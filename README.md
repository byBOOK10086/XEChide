# XEChide

[![ci](https://github.com/byBOOK10086/XEChide/actions/workflows/ci.yml/badge.svg)](https://github.com/byBOOK10086/XEChide/actions/workflows/ci.yml)

**App and path hiding with three delivery forms and one configuration.**

| Form | How it runs | Requirements | Status |
|---|---|---|---|
| **Module** (recommended for most devices) | native **Zygisk** module; a freestanding `arm64-v8a.so` is injected into app processes | Magisk's Zygisk, or **Zygisk Next** on KernelSU/APatch; Android 11+ | builds & zip-lints in CI |
| **KPM** (native APatch) | a real KernelPatch Module (`.kpm`) loaded by APatch/KernelPatch's KPM engine | APatch / KernelPatch with its KPM runtime; no custom kernel image | builds & KPM-checks in CI |
| **Kernel** (optional, advanced) | the original patch to KernelSU `0faec0cc`; filtering happens in the kernel | a kernel built from the patched KernelSU tree | patch checked against the pinned upstream; no device run yet |

All three forms consume the **same packed configuration** (`kernel/xechide.h`,
8976 bytes) and are driven by the same manager app. The module form needs **no
LSPosed**, the KPM form needs **no custom kernel image**, and the kernel form
leaves the target process completely stock.

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

## KPM form (native APatch)

`kpm/` is a **real KernelPatch Module**, built as an ET_REL AArch64 ELF with the
public KernelPatch KPM SDK (pinned commit, fetched at build time) and loaded by
APatch/KernelPatch's KPM engine. It is the kernel-form enforcement without
building a kernel:

- hooks `binder_transaction` / `binder_thread_read` (6-arg signature) and
  rewrites requests/replies with the same `kernel/xechide_parcel.c` used by the
  kernel patch;
- hooks `openat`, `newfstatat`, `faccessat`/`faccessat2`, `readlinkat`, `statx`,
  `openat2` and `getdents64` for the current uid, hiding configured paths and
  directory entries;
- reads the same `/data/adb/xechide/config`, validates it, and exposes
  `reload`/`status` through `KPM_CTL0` (APatch's KPM control dialog).

It is **not** a script wrapper and **not** an LKM. KernelSU's own `CONFIG_KPM`
loader (`sukisu_handle_kpm`) only provides a small symbol table and does not
provide the KernelPatch `hook_*` runtime; this KPM intentionally targets
APatch/KernelPatch. The Zygisk module remains the KernelSU path.

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
module/      Zygisk module + native KPM packaging (freestanding .so, root companion)
kpm/         real KernelPatch Module (KPM): source, SDK build, checker
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
# -> module/out/xechide-module-vX.zip (Zygisk .so + xechide.kpm), linted
```

KPM only (builds against the pinned KernelPatch SDK; needs network or `KP_SRC`):

```sh
ZIG=/path/to/zig bash kpm/build.sh
# -> kpm/out/xechide.kpm, validated by kpm/tools/kpm_check.py
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

### KPM form (APatch)

1. Install APatch/KernelPatch and open the KPM page.
2. Install `kpm/out/xechide.kpm` (or let the XEChide module's installer copy it
   to `/data/adb/ap/kpm/xechide/xechide.kpm`), then reboot/load it.
3. The same manager writes `/data/adb/xechide/config`; the root helper also
   mirrors it into APatch's private data (0600, APatch uid) so the APatch-app
   KPM loader/control can read it. **Reboot** or send `reload` in the APatch
   KPM control to load the new rules.
4. 64-bit app processes only for the path syscall hooks; the binder hooks are
   ABI-independent.

KernelSU's `CONFIG_KPM` loader is **not** a supported host for this KPM (it
does not expose the KernelPatch hook runtime). Use the Zygisk module there.

### Kernel form


Apply the patch, build/flash the kernel, then use the same manager and helper
(`apply` writes both the module file and the kernel ioctl when the patch is
present).

## Status and honesty

- The Zygisk `.so` is **built** (AArch64, ET_DYN, no `DT_NEEDED`, two exported
  Zygisk entries) and the zip is machine-linted; the manager APK builds.
- The KPM is **built against the real KernelPatch SDK** (ET_REL, `.kpm.info`,
  `.kpm.init/ctl0/exit` sections, only exported runtime symbols; no
  `kf_filp_*` non-exported imports) and validated by `kpm_check.py`.
- The kernel patch applies cleanly to the pinned stock KernelSU commit.
- Host tests and fuzzing cover the pure matching/parcel code shared by all
  forms. **Nothing has been run on a device yet**; `docs/verification.md` is the
  checklist before any claim gets stronger.
- The module form cannot hide files from a detector that uses a **direct
  syscall** (`syscall(SYS_openat)`) or its own late-loaded native library; the
  kernel form can. See `SECURITY.md`.

## Licence & acknowledgements

Userspace code (`manager/`, `module/`, `ctl/`, `tools/`, tests, UI) is
**GPL-3.0-only** (root [`LICENSE`](LICENSE), copy in
[`LICENSES/GPL-3.0-only.txt`](LICENSES/GPL-3.0-only.txt)). Kernel-only files
are **GPL-2.0-only**
([`LICENSES/GPL-2.0-only.txt`](LICENSES/GPL-2.0-only.txt)), matching the kernel
they are built into. The KPM form is **GPL-2.0-or-later**
([`LICENSES/GPL-2.0-or-later.txt`](LICENSES/GPL-2.0-or-later.txt)) and is built
against the GPL-2.0-or-later KernelPatch SDK. The shared ABI and pure
matching/parcel files are **GPL-2.0-only OR GPL-3.0-only** so the kernel, KPM
and userspace module can build the same source without relicensing it. The
full split is in [`NOTICE.md`](NOTICE.md) and is enforced by
`tools/license_check.py`.

XEChide builds on public interfaces from **KernelSU**, **KernelPatch/APatch**,
**Magisk/Zygisk**, **LSPlt**, the **Linux kernel**, **AOSP/AndroidX/Compose**
and **Zig**; HMA-oss is the project we compare against. Thanks to all of them —
the detailed list is in [`ACKNOWLEDGEMENTS.md`](ACKNOWLEDGEMENTS.md)
(中文/English). No third-party hiding code was copied.

