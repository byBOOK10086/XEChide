# verification

What is proven automatically, what must be proven on a device, and what has not
been proven at all. Nothing in this file is aspirational: CI runs the first
table, the second table is a checklist for a human with a phone.

## 1. Automatic (CI, every push)

| Check | Command | Proves |
|---|---|---|
| pure logic | `make -C tests/host` | path/dirent matching and parcel rewriting behave as specified on real parcel layouts |
| patch | `tools/patchcheck.sh` | `patches/xechide-core.patch` applies to stock KernelSU `0faec0cc7b5f...` |
| helper | `cc -std=c11 -Wall -Wextra -Werror ctl/xechide-ctl.c` | the ioctl ABI and the packed struct compile without warnings |

CI cannot compile the LSM/kprobe glue: that needs a kernel tree per KMI. The
first build is the acceptance test for those files.

## 2. On-device checklist

Prereqs: a KernelSU kernel with the patch applied, the manager installed, the
helper at `/data/adb/xechide/bin/xechide-ctl`, and one app marked hidden with a
rule whose target is uid 2000 (shell) so `adb` can play the detector.

```
### 2.1 hooks installed
adb shell su -c dmesg | grep xechide
  expect: "xechide: 5/5 L0 hooks installed"
  expect: "xechide: 2/2 binder probes installed"
  any "hook failed" line means that layer is off; the device still boots.

### 2.2 shell cannot list the package
adb shell pm list packages | grep com.example.hidden
  expect: no output

### 2.3 shell cannot stat the APK
adb shell ls -la /data/app | grep com.example.hidden
  expect: no output ("no permission" from SELinux is also acceptable; the
  relevant case is a uid that SELinux *does* allow, i.e. shell listing
  /sdcard/Android/data)

### 2.4 shell cannot see the data / external dirs
adb shell ls -la /sdcard/Android/data | grep com.example.hidden
  expect: no output

### 2.5 the hider is not visible
adb shell ls -la /data/adb/xechide
  expect: "No such file or directory"

### 2.6 counters move
adb shell su -c '/data/adb/xechide/bin/xechide-ctl stats'
  expect: parcel_rebuilt or parcel_rewritten increases after 2.2, path_hits
  increases after 2.3/2.4, lsm_hooks == 5, binder_hooks == 2

### 2.7 root sees everything (by design)
adb shell su -c 'pm list packages | grep com.example.hidden'
  expect: the package is listed. XEChide does not and cannot hide from root.

### 2.8 fail-open
Reboot with the manager disabled (freeze it) and check that the device boots and
that nothing is hidden: an empty configuration must change no behaviour.
```

## 3. What has NOT been done

- The patched kernel has **never been compiled** in this repository's CI. It was
  written against the KernelSU tree at the pinned commit and against
  `lsm_hook_defs.h` from 5.10 through 6.18, but the first real compile happens
  on a build machine.
- The manager has **never been run** on a device (see `manager/README.md`).
- No detector has been run against the result. When one is, its output goes in
  this file - including the leaks.

## 4. Known measurement gaps

| Question | How to measure |
|---|---|
| app cold-start overhead for a target uid | `am start -W` 20x with and without rules; the L0 path resolution is the only per-call cost |
| parcel rewrite cost | `xechide-ctl stats` deltas around one `pm list packages` |
| list removal success rate | `parcel_rebuilt` vs `parcel_rewritten` after a session: a high rewritten share means the device's `PackageInfo` encoding defeated the structural parser and the fallback rename is doing the work |
