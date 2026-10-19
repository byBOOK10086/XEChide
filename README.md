# XEChide

**Kernel-level app & path hiding for KernelSU.** Nothing is injected into the
target process, and `PackageManager` replies are filtered **inside the kernel**
before a detector ever sees them. Lower level than HMA-oss.

> XEChide is a **standalone project**: its own kernel patch, its own manager app,
> its own config format. It does not depend on, share code with, or ship inside
> any other KernelSU fork or module.

Status: design + working kernel rule engine (2026-10). Two pieces remain: the
per-KMI binder filter and the manager app.

## Why another hiding tool

Every userspace hider — HMA-oss included — hooks the **detection app's own
process**. Whatever it hooks (Java methods, libc, the class loader), the detector
can inspect itself and find the hook. Hiding becomes a race, and the hook always
leaves something to find.

XEChide moves the hiding out of the process:

| Layer | Lives in | Stops |
|---|---|---|
| **L0 · LSM file layer** | kernel (`inode_permission`, `inode_getattr`, `filldir`) | direct reads of `/data/app/<pkg>*`, `/data/data/<pkg>`, `/data/media/*/Android/data/<pkg>`, `/proc/<hidden pid>` |
| **L1 · binder reply filter** | kernel (`binder_transaction`) | `IPackageManager.getInstalledPackages`, `getPackagesForUid`, `queryIntent*`, `getApplicationInfo` replies |
| **L2 · self-hiding** | kernel (same LSM layer, fixed deny-list) | XEChide's own config/log dir from every non-root uid |
| **L3 · shell tightening** | kernel (per-uid rules) | `adb shell pm list packages`, `ls /data/app` — uid 2000 is just another target |

A detector cannot find a hook in its own process because there is none: the
kernel has already removed the data before it arrives.

## The four problems this design fixes

1. **Plaintext hide list** — the old design wrote a JSON list under `/data/adb`,
   so any root scanner could read *what* was hidden and *that* something hides.
   XEChide keeps rules in kernel memory and persists them AES-256-GCM (0600,
   path hidden by L2).
2. **Userspace injection** — LSPosed/Zygisk hooks live inside the target process
   (`maps`, ART headers, JNI tables). XEChide injects nothing.
3. **Java-only hiding** — hooking PMS Java methods misses direct binder calls,
   the `pm` CLI, and plain file access. L0/L1 catch exactly those.
4. **The hider's own traces** — module dir, config, logs, worker processes. L2
   hides them with the same rules, for every non-root uid.

Design details: [`DESIGN.md`](DESIGN.md) · threat model: [`SECURITY.md`](SECURITY.md) ·
comparison with HMA-oss: [`docs/comparison-hma.md`](docs/comparison-hma.md).

## Repository layout

```
patches/     unified diff against stock KernelSU + GKI kernels (the kernel part)
manager/     standalone Android app (own package id, own JNI, no shared code)
tools/       verification scripts: what a detector can still see
docs/        design, threat model, verification
```

## Install (once built)

1. **Kernel**: apply `patches/xechide-*.patch` to your KernelSU kernel source and
   build the image as usual (`patches/README.md` lists the supported KMIs).
2. **Manager**: install `manager/` (package `dev.xechide.manager`), pick hidden
   packages and the apps that must not see them, apply.
3. **Verify**: `tools/probe/` reproduces what a detector sees on your device.

## License

GPL-3.0 for the project; the kernel part is GPL-2.0-only like the kernel it is
built into. See [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md). No third-party
hiding code was copied.
