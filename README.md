# XEChide

**Kernel-level app & path hiding for KernelSU.** Nothing is injected into the
target process, and `PackageManager` replies are filtered **inside the kernel**
before a detector ever sees them. Lower level than HMA-oss.

> XEChide is a **standalone project**: its own kernel patch, its own manager app,
> its own config format. It does not depend on, share code with, or ship inside
> any other KernelSU fork or module.

Status (2026-10): kernel side done - one patch, L0 (four LSM entries + a
readdir filter) and L1 (two binder probes), no per-KMI code; the pure matching
and parcel code is compiled and unit-tested on the host by CI, and the patch is
checked against the pinned stock KernelSU commit. The root helper (`ctl/`) and
the manager app (`manager/`) are written and compile-checked where possible, but
**nothing has been built against a real kernel or run on a device yet**. The
docs say so instead of pretending otherwise; `docs/verification.md` is the
checklist that has to pass before any claim gets stronger.

## Why another hiding tool

Every userspace hider — HMA-oss included — hooks the **detection app's own
process**. Whatever it hooks (Java methods, libc, the class loader), the detector
can inspect itself and find the hook. Hiding becomes a race, and the hook always
leaves something to find.

XEChide moves the hiding out of the process:

| Layer | Lives in | Stops |
|---|---|---|
| **L0 · LSM file layer** | kernel (`inode_permission`, `inode_getattr`, `filldir`) | direct reads of `/data/app/<pkg>*`, `/data/data/<pkg>`, `/data/media/*/Android/data/<pkg>`, `/proc/<hidden pid>` |
| **L1 · binder parcel filter** | kernel (`binder_transaction`, `binder_thread_read`) | `IPackageManager`, `IActivityManager`, `IActivityTaskManager`, `IUsageStatsManager`, `IAppOpsService` - replies are filtered, requests are rewritten |
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
patches/     one unified diff against stock KernelSU (the whole kernel part)
kernel/      the sources that diff adds: rule engine, LSM layer, binder layer
ctl/         root helper: the only user of the three ioctls
manager/     standalone Android app (own package id, own code, no shared code)
tools/       verification scripts: what a detector can still see
tests/       host tests for the matching/parcel code, run by CI
docs/        design, threat model, config protocol
```

## Install (once built)

1. **Kernel**: apply `patches/xechide-core.patch` to your KernelSU kernel source
   and build the image as usual (`patches/README.md` has the exact base commit).
2. **Manager**: install `manager/` (package `dev.xechide.manager`), pick hidden
   packages and the apps that must not see them, apply.
3. **Verify**: `tools/probe.sh` reproduces what a detector sees on your device.

The kernel part is verified by CI in two ways: the pure matching/parcel code is
compiled and executed on the host, and the patch is checked against the pinned
stock KernelSU commit. The LSM/kprobe glue needs a real kernel tree to compile;
that is the acceptance test on the build machine, not something this repository
claims to have run.

The manager cannot talk to the kernel by itself - only the KernelSU manager
holds a driver descriptor - so it drives a small root helper (`ctl/`) that
installs one and pushes the configuration.

## License

GPL-3.0 for the project; the kernel part is GPL-2.0-only like the kernel it is
built into. See [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md). No third-party
hiding code was copied.
