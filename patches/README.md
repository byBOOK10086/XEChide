# Patches

XEChide's kernel part is a **self-contained patch set**, not a fork. Two files:

| File | Applies to |
|---|---|
| `xechide-core.patch` | stock KernelSU kernel tree (`kernel/` of `tiann/KernelSU`) on a GKI 5.10 / 5.15 / 6.1 / 6.6 kernel: adds `kernel/xechide.c`, `kernel/xechide.h`, the Kbuild line, the LSM hook calls, and the two supercall entries |
| `xechide-binder-<kmi>.patch` | the per-KMI binder filter (`binder.c` differs between 5.10/5.15/6.1/6.6); applied only for the KMI you build |

`xechide-core.patch` is the part that works today (rule engine + LSM layer);
the binder patches are written per KMI because `binder_transaction` has a
different signature on each, and each patch registers its kprobe in a
**fail-open** way: if the registration fails, the device keeps working with the
file layer only (audited via `dmesg`).

## What the core patch changes

```
kernel/Kbuild                     + kernelsu-objs += xechide.o
kernel/xechide.c                  new (rule engine, RCU config, hot-path match)
kernel/xechide.h                  new (prototypes)
kernel/core/init.c                + xechide_init() / xechide_clear()
kernel/hook/lsm_hook.c            + xechide_inode_permission / _getattr / filldir
kernel/supercall/dispatch.c       + KSU_IOCTL_XECHIDE_GET / _SET
uapi/supercall.h                  + the two ioctls and struct xechide_cmd
```

No Kconfig symbol, no new module, no external dependency. The patch keeps
XEChide **inside `kernelsu.ko`** on purpose: a separate `.ko` shows up in
`/proc/modules` and `/sys/module`, which is a fingerprint.

## Claimed ABI

XEChide owns a documented slice of the supercall ioctl space so several
KernelSU-based projects can coexist on one kernel:

```
KSU_IOCTL_XECHIDE_GET   = _IOC(_IOC_READ,  'K', 0xE0, 0)
KSU_IOCTL_XECHIDE_SET   = _IOC(_IOC_WRITE, 'K', 0xE1, 0)
```

Both are `perm = only_manager`: only the KernelSU manager (the authenticated
app) may call them. The manager talks to these ioctls **directly over the
KernelSU fd with its own JNI** — it does not need `ksud`, and it does not import
anything from any fork.

## Build matrix

| KMI | core | binder |
|---|---|---|
| android12-5.10 | ✓ | todo |
| android13-5.10 | ✓ | todo |
| android13-5.15 | ✓ | todo |
| android14-5.15 | ✓ | todo |
| android14-6.1 | ✓ | todo |
| android15-6.6 | ✓ | todo |
| android16-6.12 | ✓ | todo |
| android17-6.18 | ✓ | todo |

## Rules for patch contributions

- Never deny uid 0 / 1000 / init.
- Reads only; writes always allowed.
- Deny returns `-ENOENT`, never `-EACCES`.
- Every hook must pass through untouched on any malformed input.
- A new file must not change behaviour when `n_rules == 0`.
