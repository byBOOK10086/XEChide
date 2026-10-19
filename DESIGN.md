# XEChide — design

This document is the engineering contract. Every guarantee below is either
implemented in `kernel/` or explicitly listed as *not* provided.

## 0. Terms

- **hidden package** — the app that must become invisible.
- **target** — the app that must not see the hidden package (the detector, the
  game, a specific banking app, `adb shell`).
- **rule** — `(target_uid, hidden_uid, hidden_pkg)`; rules are the only thing
  that changes behaviour.
- Everything runs in the KernelSU kernel module. No userspace process is
  involved in enforcement, so killing the manager changes nothing.

## 1. L0 — file layer

Goal: the target cannot read the hidden app's files, and cannot learn that they
exist.

| Hook | Mechanism | Covers |
|---|---|---|
| `inode_permission` | `ksu_lsm_hook()` slot replacement | open/exec/access decisions |
| `inode_getattr` | same | `stat`/`lstat` of the path itself |
| `file_permission` | same | read/write on an already open descriptor |
| `file_open` | same | `open`, `openat`, `execve` |
| `dir_context.actor` | `iterate_dir` kretprobe + actor swap | `getdents`/`getdents64` listings |

The replacement runs before the original SELinux handler and calls it for
everything it does not deny, so SELinux enforcement is never weakened.
`inode_permission` carries no path, so one is resolved with `d_find_alias()` +
`dentry_path_raw()` — only for uids that actually have rules, and only for
read/exec/access masks.

Matching covers the layouts Android really uses:

| Class | Example |
|---|---|
| APK | `/data/app/~~a1b2/<pkg>-aBcD==/`, `/data/app/<pkg>-aBcD==/` |
| data | `/data/data/<pkg>/`, `/data/user/<n>/<pkg>/`, `/data/user_de/<n>/<pkg>/` |
| obb/media | `/data/media/<n>/Android/data/<pkg>/`, `/storage/emulated/<n>/...`, `/sdcard/Android/data/<pkg>` |
| proc | `/proc/<pid>/` where `<pid>`'s uid equals the rule's `hidden_uid` |

Hard rules:

- **Never** match uid 0 or 1000; denying system_server its own files bricks the
  boot.
- **Never** deny writes; only read/exec/getattr/readdir.
- Deny with `-ENOENT`, never `-EACCES`: the latter tells the detector
  "something exists here but you may not look".
- A path that cannot be resolved is **allowed** (fail-open).

## 2. L1 — binder

This is what makes XEChide lower than every userspace hider: there is no hook
inside the detector's process, and the data is edited in the kernel.

Two probes, neither of which reads a binder-internal structure:

1. **`binder_transaction` (request side).** Runs in the caller's context, so
   `current_uid()` is the target uid. When the parcel carries the
   `android.content.pm.IPackageManager` interface token, the thread is marked
   and package names in the request are renamed in place, same length. A
   `getPackageInfo("hidden")` therefore reaches PMS as a request for a package
   that does not exist, and PMS itself produces a genuine
   `NameNotFoundException`.
2. **`binder_thread_read` (reply side).** Also runs in the caller's context. If
   the thread is marked and the read buffer contains `BR_REPLY`, the reply
   parcel is rewritten on a private copy before the process ever parses it.

Reply rewriting has two strategies, in order:

1. **Structural removal.** If the parcel is a `List<PackageInfo>` or a
   `String[]`, every element boundary is derived from the parcel itself. For a
   package list the element start is recognisable (presence `int32 1` followed
   by the package-name string, in either String16 or String8 encoding), the
   element count must match the container header exactly, and only then is the
   element removed and the count fixed.
2. **Length-preserving rename.** Anything else — a `List<ResolveInfo>`, a
   request parcel, a list whose shape did not validate — gets the hidden name
   replaced by a same-length name (`com.example.app` → `qqq.qqqqq.qqq`). The
   parcel layout does not change at all, so this cannot desynchronise the
   receiver.

`offsets_size != 0` (the parcel carries binder objects or file descriptors) is
always passed through untouched.

Which calls are covered: every `IPackageManager` transaction, both directions.
That is intentionally *not* a table of AIDL transaction codes, which change per
Android release; the interface token and the structure of the parcel are the
selector instead.

## 3. L2 — self-hiding

The same L0 code hides the project's own artefacts, for every uid except 0 and
1000:

```
/data/adb/xechide/            encrypted config, helper binary, logs
```

The kernel creates no process, no socket and no `/proc` entry. The manager
talks to the kernel through a root helper that installs a KernelSU driver
descriptor and issues three ioctls (see `docs/config-protocol.md`).

## 4. Config protocol

A fixed packed struct (`kernel/xechide.h`), validated and published with one
RCU swap. The wire format, the ioctl numbers and the validation order are in
[`docs/config-protocol.md`](docs/config-protocol.md).

Summary: `SET` replaces everything, `GET` reads it back, `STATS` returns the
counters; all three are root-only. Invalid input leaves the previous rules in
force. An empty configuration hides nothing.

## 5. What XEChide does not do

- It does not modify `system_server`, PMS, or any framework jar.
- It cannot hide a package from an app that **has root** — root reads kernel
  memory. Out of scope by definition.
- It cannot hide the `install`/`uninstall` broadcast for a package installed
  *after* the rule is applied.
- It does not hide a package's **notifications**, which system_server posts as
  the hidden app.
- It does not hide the package from `dumpsys package` run as root.
- It does not fake `Play Integrity`.
- When reply rewriting falls back to a rename, a detector that cross-checks
  every listed package with `getPackageInfo()` will see one name that does not
  resolve. Structural removal (the preferred path) does not have this problem;
  the fallback exists because a corrupted parcel is far worse than a suspicious
  one.

## 6. Performance

- L0: one rule-bitmap test per hook; path resolution only for uids that have
  rules. Readdir: one kretprobe entry per `getdents`, one hash probe per entry.
- L1: binder probes see every transaction, but the request path is a single
  `xechide_uid_has_rules()` test for non-target uids. Parcel work happens only
  for target uids, on a preallocated buffer, protected by `spin_trylock` — if
  another filter is in flight the parcel is passed through untouched.
- A target uid with no rules configured costs one RCU dereference.

## 7. Failure policy

| Failure | Behaviour |
|---|---|
| `selinux_*` symbol missing | that hook is skipped; the rest install |
| `iterate_dir` / binder probes fail | that layer is off, the other still works |
| `d_path` failure | allow |
| Parcel does not validate | send it unchanged |
| Config invalid / decrypt failure | zero rules, previous config kept |
| Bounds | every read goes through a checked helper; nothing is written to userspace except the reply that was just validated |

## 8. Security posture

Deny decisions are only ever *added* for the hidden paths of a configured
target uid. Nothing in the design shortens SELinux checks, and no code path
returns success where the kernel would have returned an error: the hooks can
only turn an allowed operation into `-ENOENT`, never the other way around.
