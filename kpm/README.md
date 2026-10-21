# kpm/ — native KernelPatch Module (APatch)

This directory builds a **real KPM**, not a wrapper around a shell script and
not an LKM. The output is an ET_REL AArch64 ELF with the KernelPatch module
metadata and lifecycle sections (`.kpm.info`, `.kpm.init`, `.kpm.ctl0`,
`.kpm.exit`), loaded by APatch/KernelPatch's KPM engine.

It is the kernel-enforcement form without rebuilding a kernel:

| Layer | Mechanism |
|---|---|
| binder | `hook_wrap` on `binder_transaction` (5 args) and `binder_thread_read` (6 args); the same `kernel/xechide_parcel.c` used by the kernel patch |
| paths | `hook_syscalln` on `openat`, `newfstatat`, `faccessat`/`faccessat2`, `readlinkat`, `statx`, `openat2`; returns `-ENOENT` for a target uid |
| directories | `getdents64` after-hook; removes entries whose name equals a hidden package |
| config | `/data/adb/xechide/config` (same 8976-byte struct); `KPM_CTL0` commands `reload`, `status`, `version` |

Everything fails open. A missing kernel symbol, a failed hook, an invalid
config or an unknown parcel leaves the original behaviour untouched.

## Requirements and scope

- **APatch / KernelPatch** with its KPM runtime. This is the form the KPM
  ecosystem targets.
- **KernelSU is not a supported host for this KPM.** KernelSU's own
  `CONFIG_KPM` loader (`sukisu_handle_kpm`) resolves only a small symbol table
  and does not provide the KernelPatch `hook_*` runtime; the module would fail
  to resolve `hook_syscalln`/`hook_wrap`. On KernelSU use the Zygisk module or
  the kernel patch.
- Path syscall hooks cover **64-bit** app processes; the binder hooks are
  ABI-independent. 32-bit compat syscall hooks are not implemented yet.
- `getdents64` filters by **package name for the current target uid** without
  resolving the directory path, so a file literally named like a hidden package
  in an unrelated directory is also hidden. This is deliberate and documented;
  it does not affect other uids.

## Build

```sh
ZIG=/path/to/zig bash kpm/build.sh
# -> kpm/out/xechide.kpm
```

The script fetches the pinned KernelPatch source
(`052c73f93dbb84861218086e60a9962c48ff3f7f`) for its public KPM headers, then
uses Zig's clang/lld as a `aarch64-freestanding` compiler. No Android NDK and no
kernel source tree are needed. `KP_SRC=/path/to/KernelPatch` skips the download.
`build.ps1` is the Windows equivalent.

`kpm/tools/kpm_check.py` verifies the ELF kind, metadata, lifecycle sections,
relocations and imports. It specifically rejects imports of `kf_filp_open`,
`kf_kernel_read` and `kf_filp_close`, which the SDK declares but does not
export (the module resolves those through `kallsyms_lookup_name` itself).

## Install (APatch)

APatch expects a module at `/data/adb/ap/kpm/<name>/<name>.kpm`:

```sh
mkdir -p /data/adb/ap/kpm/xechide
cp xechide.kpm /data/adb/ap/kpm/xechide/xechide.kpm
chmod 600 /data/adb/ap/kpm/xechide/xechide.kpm
# reboot, or load it from the APatch KPM screen
```

The XEChide module zip bundles this file; its `customize.sh` copies it into the
APatch store when installed under APatch. The APatch KPM screen can also install
the `.kpm` directly.

After changing rules in the manager, **reboot** so `KPM_INIT` reads the new
config as root. APatch's control dialog can send:

```
reload
```

The KPM first reads the root-only `/data/adb/xechide/config`, then
`/data/data/me.bmax.apatch/files/xechide.cfg`. The root helper and the module
scripts mirror the packed config into the APatch app's private data (0600,
APatch uid) because APatch loads/controls KPMs from the manager app's uid,
which cannot traverse `/data/adb`. The private copy is only readable by the
APatch manager and root, so the hide list does not leak to ordinary apps.
`status`/`version` work regardless of the file.

## Files

```
src/xechide_kpm.c    lifecycle, config, binder hooks, syscall hooks
compat/linux/types.h KPM SDK type shim (the SDK uses <ktypes.h>)
xechide.lds          the same NOLOAD placeholders as KernelPatch's demo
build.sh / build.ps1 pinned-SDK build (Zig clang + lld)
tools/kpm_check.py   static KPM format validator
```

## Licence

The KPM source is **GPL-2.0-or-later**, compatible with the KernelPatch SDK and
the kernel. It shares `kernel/xechide.h`, `xechide_match.c` and
`xechide_parcel.c` (dual GPL-2.0-only OR GPL-3.0-only) with the other forms.
KernelPatch/APatch are GPL-2.0 and are acknowledged in
[`../ACKNOWLEDGEMENTS.md`](../ACKNOWLEDGEMENTS.md). The SDK headers are fetched
at build time and are not vendored.
