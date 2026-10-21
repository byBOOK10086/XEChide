# License texts

This directory contains the complete license texts referenced by the SPDX
headers in this repository.

| File | Used for |
|---|---|
| `../LICENSE`, `GPL-3.0-only.txt` | userspace: `manager/`, `module/` (except shared files), `ctl/`, `tools/`, `tests/`, `ui/`, docs |
| `GPL-2.0-only.txt` | kernel-only: `kernel/xechide_core.c`, `kernel/xechide_lsm.c`, `kernel/xechide_binder.c` |
| both | shared: `kernel/xechide.h`, `kernel/xechide_match.c`, `kernel/xechide_parcel.c` are dual `GPL-2.0-only OR GPL-3.0-only` |

The kernel form is compiled under GPL-2.0 like the kernel it is built into; the
module, helper, host tests and manager are GPL-3.0-only. The dual expression on
the shared ABI/logic files is what makes both builds possible without
relicensing or duplicating code.

Third-party licenses are **not** bundled here because no third-party code is
vendored: LSPlt (LGPL-3.0) runs inside the installed Zygisk, AndroidX/Compose
(Apache-2.0) are Gradle/Maven dependencies, and HMA-oss is only compared
against. See `../NOTICE.md` and `../ACKNOWLEDGEMENTS.md`.
