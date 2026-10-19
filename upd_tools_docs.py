p=r'C:\Users\Lenovo\Desktop\kernelSU\XEChide\tools\README.md'
s=open(p,encoding='utf-8').read()
s=s.replace('''- `patchcheck.sh` — verifies that `patches/xechide-core.patch` still applies to
  the pinned stock KernelSU commit without downloading the whole history. CI
  runs the same check.''','''- `patchcheck.sh` — verifies that `patches/xechide-core.patch` still applies to
  the pinned stock KernelSU commit without downloading the whole history. CI
  runs the same check.
- `abi_check.py` — cross-checks the packed ABI everywhere it is encoded (the
  kernel header, the manager's Kotlin constants and codec stride, the helper's
  ioctl numbers, the patch, the protocol doc, and the package-name charset).
  This is the check that would have caught the rule-stride bug by itself.''')
open(p,'w',encoding='utf-8',newline='').write(s)

p=r'C:\Users\Lenovo\Desktop\kernelSU\XEChide\docs\verification.md'
s=open(p,encoding='utf-8').read()
s=s.replace('''| helper | `cc -std=c11 -Wall -Wextra -Werror ctl/xechide-ctl.c` | the ioctl ABI and the packed struct compile without warnings |''','''| helper | `cc -std=c11 -Wall -Wextra -Werror ctl/xechide-ctl.c` | the ioctl ABI and the packed struct compile without warnings |
| ABI | `python3 tools/abi_check.py` | the kernel header, the Kotlin codec, the helper and the patch all agree on numbers, sizes and the package-name charset |
| arm64 helper | zig `-target aarch64-linux-musl -static` | the helper really builds for the target the APK bundles |''')
open(p,'w',encoding='utf-8',newline='').write(s)
print('docs updated')
