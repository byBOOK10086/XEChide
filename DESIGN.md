# XEChide — design

This document is the engineering contract. Every guarantee below is either
implemented in `kernel/` or explicitly listed as *not* provided.

## 0. Terms

- **hidden package** — the app that must become invisible.
- **target** — the app that must not see the hidden package (the detector, the
  game, a specific banking app, `adb shell`).
- **rule** — `(target_uid, hidden_uid, hidden_pkg)`; rules are the only thing
  that changes behaviour.
- Two delivery forms share this rule ABI and the manager:
  - **module** (default): a native Zygisk `.so` enforces in the target's process
    through PLT hooks (section 9);
  - **kernel** (optional): the KernelSU patch enforces in the kernel (sections
    1–8 below). No userspace process is involved in enforcement, so killing the
    manager or the module changes nothing.

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
| system database | `/data/system/packages.list`, `packages.xml`, `appops.xml`, `notification_policy.xml`, `package-usage.list`, `usagestats/`, `shortcut_service/`, `job/`, `users/<n>/package-restrictions.xml` (denied whole, target uids only) |

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
   `current_uid()` is the target uid. When the parcel carries one of twelve
   system-service interface tokens (`IPackageManager`, `IActivityManager`,
   `IActivityTaskManager`, `ILauncherApps`, `IPackageInstaller`,
   `IShortcutService`, `IUsageStatsManager`, `IAppOpsService`, `IJobScheduler`,
   `INotificationManager`, `IWindowManager`, `IAccessibilityManager`), the
   thread is marked and package names in the request are renamed in place, same
   length. A `getPackageInfo("hidden")` therefore reaches PMS as a request for
   a package that does not exist, and PMS itself produces a genuine
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

Real Android 11+ `IPackageManager.getInstalledPackages` replies wrap the list in
a `ParceledListSlice`, whose element layout is not self-describing; in practice
that reply usually takes the **length-preserving rename** path, not structural
removal. The counters distinguish them (`parcel_rebuilt` vs
`parcel_rewritten`), and `docs/verification.md` lists the on-device measurement
that decides whether a device can use the stronger path. This is a real
difference from HMA (which removes at the Java object level): the module form
trades the exact remove for native coverage and no Java framework dependency.

Which calls are covered: every transaction of the services above, both
directions. That is intentionally *not* a table of AIDL transaction codes, which
change per Android release; the interface token and the structure of the parcel
are the selector instead.

Package names show up in more than one shape: process names
(`com.example.hidden:remote`), flattened component names
(`com.example.hidden/.MainActivity`) and plain package names. The substitution
matches the first two by prefix and rewrites only the package part, so the
suffix - and therefore the string length - stays intact. The structural removal
path stays exact-match only, because removing a *process* from a package list
would be wrong.

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

## 9. Module form (userspace, Zygisk)

The module form exists because a kernel patch is not always an option (locked
bootloader, vendor kernel, GKI-only devices). It is a freestanding
`arm64-v8a.so` loaded by Zygisk; no LSPosed, no Java hooks, no libc++ runtime.

### 9.1 Lifecycle

1. **preAppSpecialize** (zygote privileges, before the sandbox):
   `connectCompanion()` → send `{magic, GET_CONFIG, uid, abi}` → the root
   companion reads `/data/adb/xechide/config` and returns the packed struct;
   rules whose `target_uid` matches this uid are selected. No matching rule →
   nothing is hooked at all.
2. **PLT registration**: the module finds `libbinder.so`,
   `libjavacore.so` and `libandroid_runtime.so` in `/proc/self/maps`, resolves
   their dev/inode with `stat`, and registers replacements for
   `ioctl` (libbinder) and the file API subset (libjavacore/libandroid_runtime).
   `pltHookCommit()` runs the same commit path Zygisk itself uses (lsplt).
3. **postAppSpecialize** (inside the sandbox): verify `getuid()` matches the
   expected uid, locate the module's own ELF image through maps + ELF program
   headers, remember its address range, and erase `zygisk_module_entry` /
   `zygisk_companion_entry` from `.dynstr`. From then on the image is fully
   initialised and the entry names are gone.

### 9.2 Binder filtering

The `ioctl(fd, BINDER_WRITE_READ, &bwr)` replacement calls the original, then
walks `bwr.read_buffer` with the public `BR_*` command sizes. On `BR_REPLY` it
takes the reply's `binder_transaction_data`, rejects anything with
`offsets_size != 0` or an implausible size, and runs the **same**
`xe_parcel_filter()` the kernel form uses (structural removal, else
length-preserving rename). Only `data_size` changes; the read-buffer command
lengths stay valid. A spin lock serialises the filter because the filter's
scratch arrays are static and binder replies arrive on several threads.

### 9.3 File and /proc surface

`libjavacore`'s PLT entries for `open/open64/openat/openat64`, `fopen`,
`stat/lstat/stat64/lstat64`, `access/faccessat`, `opendir/readdir/readdir64/
closedir`, `readlink/readlinkat` are replaced. The wrappers:

- return forged `ENOENT` for hidden paths (`xe_match_path()` plus the module's
  media/obb extension) and for our own `/data/adb` paths when self-hide is on;
- filter `readdir()` results for the tracked directory handles of
  `Android/{data,media,obb}` parents (with a bounded DIR* table and a spin lock);
- serve `/proc/self/maps`, `/proc/self/smaps` and `/proc/<self>/maps` through a
  streaming memfd: lines whose start address lies in the module's own image
  range are dropped (smaps blocks are dropped as a block);
- return `ENOENT` for `map_files` entries overlapping the module image.

A hook that was not installed (symbol absent) is simply never called; a missing
backup pointer is not dereferenced because the wrapper only runs after a
successful commit.

### 9.4 Failure policy

| Failure | Behaviour |
|---|---|
| companion missing / no config | module inactive in that process |
| uid has no rule | hooks are not registered |
| lib not mapped / stat fails | that library is skipped |
| commit returns false | the hooks that did apply stay; others are inactive |
| parcel shape unknown | reply unchanged |
| mprotect for `.dynstr` fails | names remain, filtering still works |
| memfd creation fails | original maps file is served |

### 9.5 SELinux and traces

The companion runs as root because `/data/adb` is root-only; app processes only
receive bytes over the socket, so no `avc` denial is generated. The module is
inherently visible inside its own process (GOT, anonymous executable mapping,
readable memory). Those traces are listed in `SECURITY.md`; the kernel form is
the answer when they matter.

## 10. KPM form (APatch / KernelPatch)

The KPM form is a real KernelPatch Module (ET_REL AArch64) loaded by the KPM
engine, so it gives kernel enforcement on APatch without rebuilding a kernel.
It is built against the public KPM SDK at pinned commit
`052c73f93dbb84861218086e60a9962c48ff3f7f` (headers fetched at build time,
GPL-2.0-or-later).

- **Config**: the same `struct xechide_config` file is read by `KPM_INIT`;
  `KPM_CTL0("reload")` re-reads it into the inactive of two static buffers and
  publishes the pointer with release/acquire atomics. Old buffers are never
  freed while hooks may still read them.
- **Binder**: `hook_wrap` on `binder_transaction` (5 args) and
  `binder_thread_read` (6 args: `proc, thread, binder_buffer, size, consumed,
  non_block`). Requests are renamed in place when the interface token matches;
  the thread is marked, and the matching `BR_REPLY` stream is filtered with the
  same `xe_parcel_filter()` as the kernel patch, including structural removal
  and its `offsets_size != 0` pass-through. A shared spin lock serialises the
  filter's static scratch; per-call copy/read scratch is separate.
- **Paths**: `hook_syscalln` before-hooks on `openat`, `newfstatat`,
  `faccessat`, `faccessat2`, `readlinkat`, `statx`, `openat2` check
  `current_uid()` and `xe_match_path()`; a hidden path sets
  `skip_origin + ret = -ENOENT`. `getdents64` is an after-hook that removes
  dirents whose name equals a hidden package for that uid.
- **Scope**: 64-bit syscall paths; binder hooks are ABI-independent. KernelSU's
  `CONFIG_KPM` engine is not a host (it does not export the KernelPatch hook
  runtime), so this form targets APatch/KernelPatch and the Zygisk module stays
  the KernelSU path.
- **Failure policy**: missing symbols, failed hooks, invalid config, unknown
  parcel shapes and failed copies all pass through unchanged.

The KPM source is GPL-2.0-or-later; the shared files remain dual-licensed and
are compiled under the GPL-2.0 option here.

## 11. Sharing one config between the forms

`xechide-ctl apply` atomically writes the packed config to
`/data/adb/xechide/config` and, if a patched KernelSU driver is present, applies
the identical bytes with `KSU_IOCTL_XECHIDE_SET`. The manager therefore does not
need a mode switch: it reports which forms are present and keeps both in sync.
The validation rules are the same as section 4; the module additionally rejects
rules whose `target_uid` does not match the process.

