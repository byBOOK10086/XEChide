# XEChide module (Zygisk)

Native `arm64-v8a` Zygisk module. One configuration drives it and the optional
kernel form; the manager app writes that configuration through the root helper.

## Requirements

- Android 11+ (API 30), 64-bit app processes.
- A Zygisk implementation that exposes **API v4** (PLT hooks by dev/inode):
  - Magisk's built-in Zygisk (Magisk 26.4+), or
  - **Zygisk Next** on KernelSU / APatch.
- No LSPosed, no Xposed, no custom kernel.

If Zygisk reports API v4 unsupported, the module refuses to initialise
(`registerModule` fails) instead of running with a mismatched table.

## Install

1. Flash `xechide-module-vX.zip` in Magisk, or in KernelSU/APatch with Zygisk
   Next. Reboot.
2. Install the manager APK (same repository, `manager/`). Install/update the
   helper from the app.
3. Pick hidden apps / targets in the manager and press **应用配置**.
4. **Restart the target apps.** A process fetches the config when it starts;
   changing rules does not retroactively update a running detector.

The config file is `/data/adb/xechide/config` (0600 root). The root companion
(`zygiskd64` hosting our handler) reads it and sends it over the companion
socket; app processes never read `/data/adb` themselves.

## What it does

- filters binder replies of `IPackageManager`, `IActivityManager`,
  `IActivityTaskManager`, `ILauncherApps`, `IPackageInstaller`,
  `IShortcutService`, `IUsageStatsManager`, `IAppOpsService`, `IJobScheduler`,
  `INotificationManager`, `IWindowManager`, `IAccessibilityManager` (the parcel
  filter is shared with the kernel form and is host-tested);
- hides `/sdcard|/storage/emulated/<n>|/data/media/<n>` `Android/{data,media,obb}`
  entries and answers `ENOENT` for the hidden app's paths (HMA does not do this);
- filters the module's own `/proc/self/maps`, `/proc/self/smaps` and
  `/proc/*/map_files` view;
- erases its own Zygisk entry names from `.dynstr` after load.

## What it does **not** do

- It cannot hide files from a detector that issues raw syscalls or loads its own
  late native library (the PLT hooks are in libjavacore/libbinder, installed
  before app libraries load).
- It does not hide a package installed after the config was applied, the app's
  notifications, `dumpsys` output, or Play Integrity verdicts.
- It does not, and cannot, match the kernel form's "no code in the process"
  property. In-process traces (`maps`, GOT, memory) remain; see `SECURITY.md`.
- 32-bit (`armeabi-v7a`) app processes are not filtered yet.

## Build

```sh
ZIG=/path/to/zig bash module/build.sh
```

No NDK is needed: Zig's clang compiles a freestanding
`aarch64-linux-android` shared object (`-nostdlib -nostdlib++`); libc symbols
are resolved from the host process at load time. The build then packages the
zip with `tools/pack_module.py` and validates it with `tools/inspect_elf.py`
and `tools/lint_module.py`.

Windows: `powershell -ExecutionPolicy Bypass -File module\build.ps1 -Zip`
(pass `-Zig <path>` if Zig is not under `_scratch/zig`).

## Files

```
module.prop        Magisk/KernelSU module metadata
customize.sh       installer hook: create the private config dir
post-fs-data.sh    fix ownership/permissions before apps start
service.sh         publish /data/adb/xechide/status for the manager
uninstall.sh       remove /data/adb/xechide
zygisk/*.so        built artifact (not committed; CI and build.sh produce it)
src/               entry + companion, libc shims, binder and file hooks
```

## Troubleshooting

- `/data/adb/xechide/status` contains `version=`, `zygisk=1`, `config=0|1`.
- No filtering at all: is the module enabled? Is Zygisk enabled? Is the target
  process 64-bit? Did you restart the target app after applying?
- `xechide-ctl status` (manager helper) prints module/config/kernel presence;
  `xechide-ctl file-get` dumps the packed config.

## Licence

The module is **GPL-3.0-only** (see the repository `LICENSE` and `NOTICE.md`).
It shares three files with the kernel form (`kernel/xechide.h`,
`xechide_match.c`, `xechide_parcel.c`) under `GPL-2.0-only OR GPL-3.0-only`;
the kernel uses the GPL-2.0 option and this module uses GPL-3.0. Thanks to
KernelSU, Magisk/Zygisk, LSPlt and the other upstreams listed in
`ACKNOWLEDGEMENTS.md`.
