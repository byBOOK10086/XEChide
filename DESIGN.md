# XEChide — design

This document is the engineering contract. Every guarantee below is either
implemented in `kernel/` or explicitly listed as *not* provided.

## 0. Terms

- **hidden package** — the app that must become invisible.
- **target** — the app that must not see the hidden package (the detector, the
  game, a specific banking app, `adb shell`).
- **rule** — `(target_uid, hidden_pkg)`; rules are the only thing that changes
  behaviour.
- Everything runs in the KernelSU kernel module. No userspace process is
  involved in enforcement, so killing the manager changes nothing.

## 1. L0 — LSM file layer

Goal: the target cannot read the hidden app's files, and cannot learn that they
exist.

Hooks (all inside `kernel/xechide.c`, registered from `kernel/core/init.c`):

```c
security_inode_permission(inode, mask)   /* read/exec/open */
security_inode_getattr(path)             /* stat of the path itself */
security_file_permission(file, mask)     /* already-open files */
dir_context.actor (`filldir`)            /* readdir must not list them */
```

Matching is done on the **inode's path** (`d_path` into a small buffer, cached
per target uid, invalidated on rename) against per-rule prefixes:

| Hidden path class | Example |
|---|---|
| APK root | `/data/app/~~a1b2/<pkg>-aBcD==/` |
| data | `/data/data/<pkg>/`, `/data/user/<n>/<pkg>/` |
| obb/media | `/data/media/<n>/Android/data/<pkg>/` |
| proc | `/proc/<pid>/` for every pid whose uid owns a hidden package |
| package.xml views | `/data/system/packages.list` (target-only deny, never for system_server) |

Hard rules:

- **Never** match `system_server` (uid 1000) or `init` — denying PMS its own
  files bricks the boot.
- **Never** deny writes; only read/exec/getattr.
- `filldir` filtering is per-directory: a single check on the opened directory
  inode (cached) plus a prefix compare on each returned name.
- If the path cannot be resolved (too long / no dentry), the call is **allowed**
  (fail-open). A missed hide is a bug report; a wrong deny is a bricked device.

## 2. L1 — binder reply filter

This is what makes XEChide lower than every userspace hider: the *data* is
removed from the binder transaction, so the target's `PackageManager` calls
return a shorter list, not a hooked one.

Hook point: `binder_transaction` (or the reply path in `binder_thread_read`) —
we need the transaction whose **destination is the package service** and whose
**sender is a target uid**. The reply parcel is a `Parcel` with
`int32 len` + body; the body for `getInstalledPackages` is
`List<PackageInfo>`: `int32 N`, then N `PackageInfo` parcels, each prefixed by
its length.

Filter algorithm (strict):

1. Read `len`, `N`, and the offset of every `PackageInfo` block; walk them once
   to build a **plan** (`keep[i]` flags). Any structural mismatch → abort.
2. If nothing is dropped, do nothing.
3. Otherwise write a new parcel: new `N`, then the kept blocks, with lengths
   unchanged (each block is self-contained). Update the outer `len` and the
   binder reply's byte count.
4. **Abort (pass through unchanged)** on: unknown service, unknown transaction
   code, non-list payload, target uid not in rules, any bounds error.

Which transaction codes: `GET_INSTALLED_PACKAGES`, `GET_PACKAGES_FOR_UID`,
`QUERY_INTENT_ACTIVITIES`/`SERVICES`/`RECEIVERS`, `GET_APPLICATION_INFO`,
`GET_PACKAGE_INFO`, `RESOLVE_INTENT`, `GET_INSTALLED_APPLICATIONS`,
`GET_APP_INFO` (legacy AIDL codes live in `IPackageManager`; they are looked up
by name at load time from `/system/framework/framework.jar`? no — they are
stable AIDL transaction numbers; see `kernel/binder_codes.h`).

Because the transaction numbers must track Android releases, the filter is
**opt-in per code** and the kernel logs an audit line (`dmesg -s`) whenever a
code is unknown, so mismatches are visible without breaking anything.

## 3. L2 — self-hiding

The kernel hides its own artefacts with the same L0 code, with a fixed
default deny-list for every non-root uid:

```
/data/adb/xechide/            (encrypted config, logs)
```

Plus: XEChide does not create a process, a socket, or a `/proc` entry. The
manager app talks to the kernel over the KernelSU supercall (`/proc` fd already
hidden by KernelSU itself).

## 4. Config protocol

- RAM: `struct xechide_config { u32 magic; u32 version; u32 n_rules;
  struct xechide_rule rules[MAX]; }` — MAX = 128 rules, each
  `{ u32 target_uid; u8 pkg[128]; u8 flags; }`.
- Transport: two new KernelSU supercalls (`KSU_IOCTL_XECHIDE_GET`,
  `KSU_IOCTL_XECHIDE_SET`), `perm = only_manager`. The manager app is the only
  caller allowed; `ksud` gets a CLI for scripting.
- Persistence: `/data/adb/xechide/cfg.bin` — AES-256-GCM, key derived from a
  random 32-byte secret written **once** at install into the KernelSU manager's
  private dir; file mode 0600, owner root. The kernel never writes it; `ksud`
  reads it at boot and pushes it through the supercall. If the file is missing
  or fails to decrypt, XEChide runs with **zero rules** (nothing hidden).
- The list is *never* in plaintext on disk and *never* exposed to a non-root
  uid (L0 hides both the dir and the file).

## 5. What XEChide does not do

- It does not modify `system_server`, PMS, or any framework jar.
- It cannot hide a package from an app that **has root** — root reads kernel
  memory.
- It cannot hide the `install`/`uninstall` broadcast for a package that is
  installed *after* the rule is applied (rules key off the package name; the
  broadcast already carries it). Workaround: apply rules before installing.
- It does not hide a package's **notifications**, which are posted by
  system_server as the hidden app (out of scope; document it).
- It does not hide the package from `dumpsys package` run as root.
- It does not fake `Play Integrity`.

## 6. Performance

- L0: one hash lookup per path operation on a hot cache (`d_path` cached by
  inode, LRU 512). Measured target: < 2 % on `app_launch` benchmarks.
- L1: binder transactions to PMS are rare (a few per app start); the walk is
  O(entries) over a 1–3 KB parcel. Zero cost when a uid has no rules (single
  bitmap test at the top of the hook).

## 7. Failure policy

| Failure | Behaviour |
|---|---|
| Unknown binder code / malformed parcel | pass through, audit once per uid |
| `d_path` failure | allow |
| Config decrypt failure | zero rules |
| Kernel panic risk (bounds) | all reads through `copy_from_user`-style checked helpers; no writes to userspace buffers except the reply rebuild, which is bounds-checked |
| Uninstall | rules vanish on reboot (kernel memory only); nothing to clean |
