# NOTICE

## Copyright

XEChide is Copyright (C) 2026 the XEChide authors and contributors.

## License boundaries

Three license groups coexist in this repository on purpose:

| Group | Files | License |
|---|---|---|
| Userspace | `manager/`, `module/` (except the shared files below), `ctl/`, `tools/`, `tests/`, `ui/`, docs | **GPL-3.0-only** — root `LICENSE`, copy in `LICENSES/GPL-3.0-only.txt` |
| Kernel-only | `kernel/xechide_core.c`, `kernel/xechide_lsm.c`, `kernel/xechide_binder.c` | **GPL-2.0-only** — copy in `LICENSES/GPL-2.0-only.txt` |
| KPM | `kpm/src/`, `kpm/compat/`, `kpm/xechide.lds` | **GPL-2.0-or-later** — copy in `LICENSES/GPL-2.0-or-later.txt` |
| Shared | `kernel/xechide.h`, `kernel/xechide_match.c`, `kernel/xechide_parcel.c` | **GPL-2.0-only OR GPL-3.0-only** |

The shared files are the packed ABI and the pure matching/parcel logic. The
kernel builds them under GPL-2.0 (the kernel's license); the Zygisk module,
helper and host tests build the same files under GPL-3.0. Each file's SPDX
header states the expression; `tools/license_check.py` enforces the split.

The patch in `patches/xechide-core.patch` adds GPL-2.0-only code to a KernelSU
tree and does not relicense KernelSU or any file it did not author. The
KernelSU base commit is recorded in `patches/README.md`.

No additional restrictions are placed on any GPL-licensed code: the complete
corresponding source is this repository.

## Upstream acknowledgements

XEChide is built on public interfaces from several upstream projects. We thank
their authors and contributors. The full list, with what is used and what is
not, is in [`ACKNOWLEDGEMENTS.md`](ACKNOWLEDGEMENTS.md).

- **KernelSU** (`tiann/KernelSU`, GPL-3.0; kernel files GPL-2.0) — the optional
  kernel form is a patch against the pinned commit
  `0faec0cc7b5f655170d89bf9c4d28bb5ca271fa2` and uses the public supercall
  dispatch table, `ksu_lsm_hook()` and manager authentication. No KernelSU
  source is copied into this repository.
- **KernelPatch / APatch** (`bmax121/KernelPatch` GPL-2.0,
  `bmax121/APatch` GPL-2.0) — the KPM form is built against the public KPM SDK
  at pinned commit `052c73f93dbb84861218086e60a9962c48ff3f7f` and its
  `hook_syscalln` / `hook_wrap` / `kallsyms_lookup_name` runtime API. The SDK
  headers are fetched at build time (not vendored); no KernelPatch/APatch
  implementation code is copied into this repository. The `.kpm` is a native
  KernelPatch module, not a wrapper around a script or an LKM.
- **Magisk / Zygisk** (`topjohnwu/Magisk`, GPL-3.0) — the module form runs as a
  Zygisk module and calls the public v4 module API (`registerModule`,
  `connectCompanion`, `pltHookRegister`, `pltHookCommit`). The ABI declarations
  in `module/src/zygisk_api.h` were written independently from the published
  interface documentation; no Magisk/Zygisk implementation code is vendored.
- **LSPlt** (`LSPosed/LSPlt`, LGPL-3.0) — the PLT hook implementation used
  *inside* Magisk's Zygisk API. XEChide calls the Zygisk API only; LSPlt code is
  not vendored, linked or modified here.
- **Linux kernel** (GPL-2.0) — LSM, kprobe and binder UAPI structures.
- **Android Open Source Project / AndroidX / Jetpack Compose / Material
  Components** (Apache-2.0) — the manager's platform and UI dependencies. They
  are build dependencies, not vendored source.
- **Zig** (`ziglang/zig`, MIT) — the compiler used to build the freestanding
  module and the static helper; a build tool only.
- **HMA-oss** — the project we compare against and learned the problem from.
  No HMA code, assets or resource files were taken; the comparison is
  behavioural, from public documentation. Thanks to its authors.

## No third-party code copied

- No HMA-oss code, assets or resource files.
- No LSPosed, Shizuku, Magisk or Zygisk implementation code is copied; the
  module contains an independent declaration of the public ABI and its own
  implementation.
- No KernelPatch or APatch implementation code is vendored. The KPM form is
  compiled against the public KernelPatch SDK headers at build time, which are
  GPL-2.0-or-later and are attributed here.
- No LSPlt code is vendored; the PLT hook engine is whatever the installed
  Zygisk provides at runtime.
- **TrickyStore** (ships without a license) and **OhMyKeymint** (AGPL-3.0) are
  not used, not vendored and not referenced by code.
- The `zygisk.hpp` sample header from topjohnwu/zygisk-module-sample is **not**
  included; `module/src/zygisk_api.h` is a minimal, independent declaration of
  the same public ABI.

## Trademarks

Android is a trademark of Google LLC. KernelSU, Magisk, Zygisk, LSPosed and
LSPlt are projects of their respective authors. This project is not affiliated
with Google, the KernelSU maintainers or the Magisk maintainers.
