# XEChide

[![ci](https://github.com/byBOOK10086/XEChide/actions/workflows/ci.yml/badge.svg)](https://github.com/byBOOK10086/XEChide/actions/workflows/ci.yml)

**App and path hiding with two delivery forms and one configuration.**

| Form | How it runs | Requirements | Status |
|---|---|---|---|
| **Module** (recommended) | native **Zygisk** module; a freestanding `arm64-v8a.so` is injected into app processes | Magisk's Zygisk, or **Zygisk Next** on KernelSU/APatch; Android 11+ | builds & zip-lints in CI |
| **Kernel** (optional, advanced) | the original patch to KernelSU `0faec0cc`; filtering happens in the kernel | a kernel built from the patched KernelSU tree | patch checked against the pinned upstream; no device run yet |

Both forms consume the **same packed configuration** (`kernel/xechide.h`,
8976 bytes) and are driven by the same manager app. The module form needs **no
LSPosed** and no custom kernel; the kernel form needs no Zygisk and leaves the
target process completely stock.

> XEChide is a **standalone project**: its own module, its own manager app, its
> own config format. It does not depend on, share code with, or ship inside any
> other KernelSU fork or module.

## What the module form actually does

HMA-oss is a **Java** Xposed module: it hooks `PackageManager` in the detector's
process through LSPosed. XEChide's module form is native and needs only Zygisk:

- **Parcel-level binder filtering** — hooks `libbinder.so`'s `ioctl` PLT entry
  and rewrites `IPackageManager` / `IActivityManager` replies *before* Java parses
  them. Native binder callers are covered too, not just Java hooks.
- **File and path hiding** — hooks `libjavacore.so` (`open`, `stat`, `access`,
  `opendir`/`readdir`, `readlink`) so the hidden app's `Android/data`,
  `Android/media`, `Android/obb` and data directories answer `ENOENT` and
  disappear from listings. HMA does not hide these.
- **`/proc/self/maps` filtering** — the module's own mappings (and `map_files`
  names) are served through a filtered memfd, so a detector reading its own maps
  does not see the injected image.
- **Self-hiding** — after loading, the two Zygisk entry-point names are erased
  from the module's `.dynstr`; the module is loaded from a `memfd` named
  `jit-cache` by Zygisk itself, so no module path appears in `maps`.
- **Root companion, not app-side file access** — the config lives in
  `/data/adb/xechide/config` (root-only). A root companion process reads it and
  hands the bytes to each app over a socket. App processes never touch
  `/data/adb`, so no SELinux policy is needed for the module.
- **Fail-open everywhere** — no config, no rule for this uid, a symbol missing
  from a library, an unparsable parcel, a hook that fails to commit: the
  process behaves exactly like stock.

In-process traces are unavoidable in this form (a GOT entry points somewhere,
an anonymous executable mapping exists, the module's memory is readable). Those
are documented in [`SECURITY.md`](SECURITY.md) rather than papered over. The
kernel form exists precisely for people who cannot accept them.

## Kernel form (optional)

The original design still lives in this repository: one patch against stock
KernelSU (`patches/xechide-core.patch`) that replaces four SELinux LSM slots and
adds an `iterate_dir` kretprobe, plus two binder probes. Nothing is injected
into any process; the kernel removes the data before it arrives. It is the
stronger form against app self-checks and the weaker form operationally (custom
kernel, kernel-version coupling, stability risk). `kernel/`, `patches/` and the
kernel half of the docs are unchanged by the module work.

## Repository layout

```
module/      Zygisk module: freestanding .so, root companion, build + zip tools
kernel/      shared packed ABI + pure matching/parcel code, kernel enforcement sources
patches/     unified diff against stock KernelSU (optional kernel form)
ctl/         stateless root helper: writes module config, sets kernel config
manager/     Android app (Compose): rules, status, apply
tests/host/  host tests + ASan/UBSan fuzz for the pure matching/parcel code
tools/       ABI cross-check and packaging/ELF validators
ui/          interactive preview of the manager screens
docs/        design, threat model, config protocol, HMA comparison
```

## Build

Module (no Android NDK; Zig 0.15.x provides clang + the freestanding target):

```sh
ZIG=/path/to/zig bash module/build.sh
# -> module/out/xechide-module-vX.zip, linted (module.prop, layout, ELF, exports)
```

Manager (JDK 17 + Android SDK 35):

```sh
cd manager && gradle :app:assembleDebug
```

Kernel form: apply `patches/xechide-core.patch` to KernelSU at the commit in
`patches/README.md`, build the kernel image as usual.

## Install

### Module form

1. Flash `module/out/xechide-module-vX.zip` in Magisk, or in KernelSU/APatch
   with **Zygisk Next** installed. Enable Zygisk / the module, reboot.
2. Install the manager APK, install/update the helper when asked, choose hidden
   apps and targets, apply.
3. **Restart the target apps** — rules are fetched when a process starts.
4. Check `/data/adb/xechide/status` (root) for the module version/config state.

Requires 64-bit (`arm64-v8a`) app processes. 32-bit apps are not filtered by the
module form yet; use the kernel form if that matters.

### Kernel form

Apply the patch, build/flash the kernel, then use the same manager and helper
(`apply` writes both the module file and the kernel ioctl when the patch is
present).

## Status and honesty

- The module `.so` is **built** (AArch64, ET_DYN, no `DT_NEEDED`, two exported
  Zygisk entries) and the zip is machine-linted; the manager APK builds.
- The kernel patch applies cleanly to the pinned stock KernelSU commit.
- Host tests and fuzzing cover the pure matching/parcel code shared by both
  forms. **Nothing has been run on a device yet**; `docs/verification.md` is the
  checklist before any claim gets stronger.
- The module form cannot hide files from a detector that uses a **direct
  syscall** (`syscall(SYS_openat)`) or its own late-loaded native library; the
  kernel form can. See `SECURITY.md`.

## Licence

GPL-3.0 for the project; the kernel part is GPL-2.0-only like the kernel it is
built into. See [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md). No third-party
hiding code was copied; the Zygisk ABI structs were written from the public
interface documentation.
