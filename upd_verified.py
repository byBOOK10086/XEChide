p=r'C:\Users\Lenovo\Desktop\kernelSU\XEChide\docs\verification.md'
s=open(p,encoding='utf-8').read()
s=s.replace('''## 3. What has NOT been done

- The patched kernel has **never been compiled** in this repository's CI. It was
  written against the KernelSU tree at the pinned commit and against
  `lsm_hook_defs.h` from 5.10 through 6.18, but the first real compile happens
  on a build machine.
- The manager has **never been run** on a device (see `manager/README.md`).
- No detector has been run against the result. When one is, its output goes in
  this file - including the leaks.''','''## 3. Builds that have actually happened

| Component | Toolchain | Result |
|---|---|---|
| host tests | Zig 0.15.2 (clang), `-DXECHIDE_HOST`, ASan+UBSan fuzz | 62 assertions + 200k fuzz iterations pass |
| root helper | Zig 0.15.2, `aarch64-linux-musl -static` | 2,731,592-byte ARM64 binary |
| manager APK | JDK 17 + Gradle 8.9 + AGP 8.7.3 + compileSdk 35 | **`BUILD SUCCESSFUL`**, 23,469,031-byte debug APK, helper bundled at `assets/xechide-ctl-arm64` |

The APK was built on 2026-10-20 with the exact sources in this repository. It is
debug-signed (`CN=Android Debug`), package `dev.xechide.manager`, versionCode 1,
minSdk 30, targetSdk 35.

## 4. What has NOT been done

- The patched kernel has **never been compiled**. It was written against the
  KernelSU tree at the pinned commit and against `lsm_hook_defs.h` from 5.10
  through 6.18, but the first real compile happens on a build machine.
- The manager has **never been run** on a device (see `manager/README.md`), and
  neither has the helper.
- No detector has been run against the result. When one is, its output goes in
  this file - including the leaks.''')
s=s.replace('''## 4. Known measurement gaps''','''## 5. Known measurement gaps''')
open(p,'w',encoding='utf-8',newline='').write(s)

p=r'C:\Users\Lenovo\Desktop\kernelSU\XEChide\manager\README.md'
s=open(p,encoding='utf-8').read()
s=s.replace('''The app has not been run on a physical device in this repository. The kernel
side is host-tested; the manager and root helper are still unverified end to
end. Do not treat any screen as proof that the kernel patch is active.''','''The app **compiles**: a debug APK was built on 2026-10-20 with JDK 17,
Gradle 8.9, AGP 8.7.3 and compileSdk 35, with the helper bundled. It has not
been run on a physical device, the helper has not been executed, and the kernel
patch has not been compiled. Do not treat any screen as proof that the kernel
patch is active.''')
open(p,'w',encoding='utf-8',newline='').write(s)
print('docs updated')
