# verification

What is proven automatically, what must be proven on a device, and what has not
been proven at all. Nothing in this file is aspirational: CI runs the first
table, the second table is a checklist for a human with a phone.

## 1. Automatic (CI, every push)

All seven jobs are green on `main` (2026-10-21, run
[39548105368](https://github.com/byBOOK10086/XEChide/actions/runs/39548105368)):
ABI cross-check, license split + upstream attribution, host tests + ASan fuzz,
root helper host build, stock-KernelSU patch apply, the aarch64-linux-musl
helper build, and the Zygisk module build + zip lint.

| Check | Command | Proves |
|---|---|---|
| pure logic | `make -C tests/host` | path/dirent matching and parcel rewriting behave as specified on synthetic parcel layouts |
| patch | `tools/patchcheck.sh` | `patches/xechide-core.patch` applies to stock KernelSU `0faec0cc7b5f...` |
| helper | `cc -std=c11 -Wall -Wextra -Werror ctl/xechide-ctl.c` | the ioctl/file ABI and the packed struct compile without warnings |
| ABI | `python3 tools/abi_check.py` | kernel header, Kotlin codec, helper, patch and module all agree on numbers, sizes, paths and the package-name charset |
| license | `python3 tools/license_check.py` | GPL-3.0 userspace / GPL-2.0 kernel / dual shared split, LICENSE texts and upstream attribution are intact |
| arm64 helper | zig `-target aarch64-linux-musl -static` | the helper really builds for the target the APK bundles |
| module | `bash module/build.sh` | the freestanding AArch64 Zygisk `.so` links with no `DT_NEEDED`, exports both entry points, and the packaged zip passes `lint_module.py` |
| KPM | `bash kpm/build.sh` | the `.kpm` is ET_REL AArch64 with `.kpm.info`/`.init`/`.ctl0`/`.exit`, pinned KernelPatch SDK, and only exported runtime imports (`kpm_check.py`) |

CI cannot compile the LSM/kprobe glue: that needs a kernel tree per KMI. The
first build is the acceptance test for those files. It also cannot run Zygisk;
the module is validated statically, not on a device.

## 2. Module-form checklist (Zygisk)

Prereqs: module zip flashed, Zygisk (or Zygisk Next) enabled, manager/helper
installed, one app hidden, target apps restarted after applying.

```
### M.1 module is installed and has a config
adb shell su -c '/data/adb/xechide/bin/xechide-ctl status'
  expect: module=1, module_so=1, config=1, config_bytes=8976
  (a stale config or module=0 means the zip/Zygisk is not active)

### M.2 the config file is the packed struct
adb shell su -c '/data/adb/xechide/bin/xechide-ctl file-get | wc -c'
  expect: 8976

### M.3 target cannot see the hidden package (Java/binder)
adb shell am start ... (run the detector in the target uid)
  expect: getInstalledPackages has no matching name; getPackageInfo returns
  null/renamed info. Because Android 11+ wraps the reply in ParceledListSlice,
  the rename fallback is usually what fires; check `parcel_rewritten` growth.

### M.4 external-storage dirs are hidden
(in the detector app) list /sdcard/Android/data and /sdcard/Android/media
  expect: the hidden package's directories are absent; stat/open return ENOENT

### M.5 maps does not show the module path
(in the detector app) read /proc/self/maps
  expect: no /data/adb/modules/xechide path; no line for the module's image
  range. A native scanner using raw syscalls can still see an anonymous
  executable mapping - that trace is documented in SECURITY.md.

### M.6 no SELinux noise from the app side
adb logcat -b events | grep -i 'avc: denied' (while running M.3/M.4)
  expect: no new denial caused by the module. The app never opens /data/adb;
  config is delivered by the root companion.
```

## 3. KPM-form checklist (APatch)

Prereqs: APatch/KernelPatch with the KPM runtime, `xechide.kpm` installed at
`/data/adb/ap/kpm/xechide/xechide.kpm`, one hidden app, and the manager helper
with a config file. This is the native KPM form; it needs no Zygisk and no
custom kernel image.

```
### K.1 the module is loaded by APatch
(APatch KPM screen) the module "xechide" is listed/loaded
  expect: name=xechide, license=GPL v2; otherwise check the KPM load error

### K.2 control/reload works
send "status" in the KPM control dialog
  expect: loaded=1 rules=<n> binder=1 path=1 getdents=1
after changing rules, send "reload" (or reboot)
  expect: reload=ok rules=<n>. The helper mirrors the config to
  /data/data/me.bmax.apatch/files/xechide.cfg (0600, APatch uid), so the
  APatch-app control can read it without traversing /data/adb.

### K.3 binder hiding
run the detector in a target uid
  expect: hidden packages absent/renamed in getInstalledPackages and
  getPackageInfo replies; binder=1 is required

### K.4 path and dirent hiding
in the target app: exists()/open() on the hidden app's Android/data dir
  expect: ENOENT; File.listFiles() on Android/data does not show the package

### K.5 no in-process trace
in the target app: read /proc/self/maps and inspect its own libs
  expect: no XEChide module image in the app process (enforcement is in the
  kernel). The KPM itself is visible to root/kallsyms, as documented.
```

## 4. Kernel-form checklist

Prereqs: a KernelSU kernel with the patch applied, the manager installed, the
helper at `/data/adb/xechide/bin/xechide-ctl`, and one app marked hidden with a
rule whose target is uid 2000 (shell) so `adb` can play the detector.

```
### 4.1 hooks installed
adb shell su -c dmesg | grep xechide
  expect: "xechide: 5/5 L0 hooks installed"
  expect: "xechide: 2/2 binder probes installed"
  any "hook failed" line means that layer is off; the device still boots.

### 4.2 shell cannot list the package
adb shell pm list packages | grep com.example.hidden
  expect: no output

### 4.3 shell cannot stat the APK
adb shell ls -la /data/app | grep com.example.hidden
  expect: no output ("no permission" from SELinux is also acceptable; the
  relevant case is a uid that SELinux *does* allow, i.e. shell listing
  /sdcard/Android/data)

### 4.4 shell cannot see the data / external dirs
adb shell ls -la /sdcard/Android/data | grep com.example.hidden
  expect: no output

### 4.5 the hider is not visible
adb shell ls -la /data/adb/xechide
  expect: "No such file or directory"

### 4.6 counters move
adb shell su -c '/data/adb/xechide/bin/xechide-ctl stats'
  expect: parcel_rebuilt or parcel_rewritten increases after 2.2, path_hits
  increases after 2.3/2.4, lsm_hooks == 5, binder_hooks == 2

### 4.7 root sees everything (by design)
adb shell su -c 'pm list packages | grep com.example.hidden'
  expect: the package is listed. XEChide does not and cannot hide from root.

### 4.8 process lists
adb shell ps -A | grep com.example.hidden
  expect: no output (readdir of /proc is filtered)

`dumpsys` is NOT covered: it writes into a file descriptor, not a parcel.
Do not use it as a leak test; see SECURITY.md item 3.

### 4.9 notifications / shortcuts / jobs
adb shell cmd notification list | grep com.example.hidden
adb shell cmd shortcut get-shortcuts --user 0 | grep com.example.hidden
adb shell cmd jobscheduler get-job-state --user 0 <target uid> <job id>
  expect: no output for the hidden package (replies come from
  INotificationManager / IShortcutService / IJobScheduler and are parcel-filtered)

### 4.10 the package databases are not a bypass
for f in packages.list packages.xml appops.xml notification_policy.xml; do
  adb shell cat /data/system/$f | grep com.example.hidden
done
adb shell ls /data/system/usagestats /data/system/shortcut_service /data/system/job
  expect: "Permission denied" or no such file for a target uid; this is
  intentional - parsing the databases must not be a way around the binder filter

### 4.11 fail-open
Reboot with the manager disabled (freeze it) and check that the device boots and
that nothing is hidden: an empty configuration must change no behaviour.
```

## 5. Builds that have actually happened

| Component | Toolchain | Result |
|---|---|---|
| host tests | Zig 0.15.2 (clang), `-DXECHIDE_HOST`, ASan+UBSan fuzz | **81 assertions** + 200k fuzz iterations pass |
| root helper | Zig 0.15.2, `aarch64-linux-musl -static` | 1,054,696-byte ARM64 binary (apply/status/file-get/KPM status + APatch mirror) |
| manager APK | JDK 17 + Gradle 8.9 + AGP 8.7.3 + compileSdk 35 | **`BUILD SUCCESSFUL`**, 23,924,635-byte debug APK, helper bundled at `assets/xechide-ctl-arm64` |
| Zygisk module | Zig 0.15.2, `aarch64-linux-android` freestanding (`-nostdlib -nostdlib++`) | 26,568-byte `.so` (ET_DYN, no `DT_NEEDED`, both entry points exported); 28,260-byte zip with `xechide.kpm`; `inspect_elf.py` + `lint_module.py` pass |
| KPM | Zig 0.15.2, `aarch64-freestanding` + pinned KernelPatch SDK | 34,504-byte ET_REL `.kpm`; `.kpm.info/.init/.ctl0/.exit` present, no non-exported `kf_filp_*` imports; `kpm_check.py` pass |

The APK was built on 2026-10-21 with the exact sources in this repository. It is
debug-signed (`CN=Android Debug`), package `dev.xechide.manager`, versionCode 1,
minSdk 30, targetSdk 35.

## 6. What has NOT been done

- The patched kernel has **never been compiled**. It was written against the
  KernelSU tree at the pinned commit and against `lsm_hook_defs.h` from 5.10
  through 6.18, but the first real compile happens on a build machine.
- The manager has **never been run** on a device (see `manager/README.md`), and
  neither has the helper.
- The Zygisk module has **never been loaded** by a real Zygisk: the ABI is
  validated statically against the published v4 layout, the ELF is machine
  checked, but `pltHookCommit()` success, companion IPC and the parcel rewrite
  path are unproven until the on-device checklist above is run.
- The KPM has **never been loaded** by a real APatch/KernelPatch KPM engine:
  the ELF format, metadata, lifecycle sections, relocation types and import
  list are statically validated; hook registration, binder arity at runtime and
  the syscall hooks still need the KPM checklist.
- No detector has been run against any form. When one is, its output goes in
  this file - including the leaks.

## 7. Known measurement gaps

| Question | How to measure |
|---|---|
| app cold-start overhead for a target uid | `am start -W` 20x with and without rules; the L0 path resolution is the only per-call cost |
| parcel rewrite cost | `xechide-ctl stats` deltas around one `pm list packages` |
| list removal success rate | `parcel_rebuilt` vs `parcel_rewritten` after a session: a high rewritten share means the device's `PackageInfo` encoding defeated the structural parser and the fallback rename is doing the work |
| module cold-start overhead | `am start -W` 20x; preAppSpecialize adds one companion round-trip plus one `pltHookCommit()` per target process |
| module maps filtering | read `/proc/self/maps` in a target app and diff against a stock build; the module image range must be absent |
| module file hiding | `File.listFiles()` on `/sdcard/Android/{data,media,obb}` in a target app; hidden package dirs must be missing and `exists()` false |
| KPM binder filter | hidden package absent from `getInstalledPackages` in a target uid; `status` shows `binder=1`; check `dmesg` for "xechide-kpm: binder=1" |
| KPM path/dirent hiding | `exists()`/`open()` on the hidden app's external dirs return ENOENT; `listFiles()` omits the package; `getdents64` after-hook path |
| KPM runtime import resolution | load the module on APatch and confirm no "unknown symbol" for `hook_syscalln`/`hook_wrap`/`kallsyms_lookup_name`; `kpm_check.py` already rejects non-exported `kf_filp_*` |
