# Patches

XEChide's kernel part is one **self-contained patch**, not a fork:

```
patches/xechide-core.patch
```

It applies to stock KernelSU (`tiann/KernelSU`) at
`0faec0cc7b5f655170d89bf9c4d28bb5ca271fa2` and adds:

```
kernel/Kbuild                + five objects under kernel/xechide/
kernel/xechide/xechide.h        ABI + internal API (new)
kernel/xechide/xechide_core.c   rule engine, RCU config, /proc check, ioctls (new)
kernel/xechide/xechide_match.c  pure path/dirent matching (new)
kernel/xechide/xechide_parcel.c pure binder parcel rewriting (new)
kernel/xechide/xechide_lsm.c    L0 hooks + readdir filter (new)
kernel/xechide/xechide_binder.c L1 probes (new)
kernel/core/init.c           + xechide_init()/_lsm_init()/_binder_init() and the exit path
kernel/supercall/dispatch.c  + the three ioctl handlers
uapi/supercall.h             + 0xE0..0xE2
```

There is **no per-KMI patch**. The binder layer never reads a binder-internal
structure: it works on the UAPI `binder_transaction_data` and on the read
stream, both of which are stable, and `current_uid()` identifies the target.
The LSM layer is version-guarded (`inode_permission` lost its `mnt_userns`
argument in 6.5) and everything else goes through KernelSU's own
`ksu_lsm_hook()`.

No Kconfig symbol, no new module, no external dependency. XEChide stays
**inside `kernelsu.ko`** on purpose: a separate `.ko` shows up in
`/proc/modules` and `/sys/module`, which is a fingerprint by itself.

## Claimed ABI

XEChide owns a documented slice of the supercall ioctl space so several
KernelSU-based projects can coexist on one kernel:

```
KSU_IOCTL_XECHIDE_GET   = _IOC(_IOC_READ,  'K', 0xE0, 0)   /* struct xechide_config  */
KSU_IOCTL_XECHIDE_SET   = _IOC(_IOC_WRITE, 'K', 0xE1, 0)   /* struct xechide_config  */
KSU_IOCTL_XECHIDE_STATS = _IOC(_IOC_READ,  'K', 0xE2, 0)   /* struct xechide_stats   */
```

All three are `perm_check = only_root`, and that is deliberate: a standalone
app cannot be the KernelSU manager, so the manager app drives a small root
helper (`ctl/`) that installs a driver descriptor through the legacy
`reboot(KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, ...)` path and then issues the
ioctls. Nothing from any fork is imported.

## Build matrix

| KMI | L0 (LSM + readdir) | L1 (binder) |
|---|---|---|
| android12-5.10 | guarded | UAPI only |
| android13-5.10 | guarded | UAPI only |
| android13-5.15 | guarded | UAPI only |
| android14-5.15 | guarded | UAPI only |
| android14-6.1 | guarded | UAPI only |
| android15-6.6 | guarded | UAPI only |
| android16-6.12 | `ksu_lsm_hook` handles static calls | UAPI only |
| android17-6.18 | `ksu_lsm_hook` handles static calls | UAPI only |

Each hook installs independently and fails open: a kernel without
`selinux_inode_permission` in kallsyms still boots and hides nothing there.

## Rules for patch contributions

- Never deny uid 0 / 1000 / init.
- Reads only; writes always allowed.
- Deny returns `-ENOENT`, never `-EACCES`.
- Every hook must pass through untouched on any malformed input.
- A configuration with `n_rules == 0` must change no behaviour at all.

## Applying

```
git clone https://github.com/tiann/KernelSU.git
cd KernelSU
git checkout 0faec0cc7b5f655170d89bf9c4d28bb5ca271fa2
git apply /path/to/xechide-core.patch
# then build the kernel/module with your usual GKI flow
```

`tests/README.md` has the exact commands CI runs to prove the patch still
applies.
