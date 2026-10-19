p=r'C:\Users\Lenovo\Desktop\kernelSU\XEChide\.github\workflows\ci.yml'
s=open(p,encoding='utf-8').read()
if 'abi cross-check' not in s:
    s=s.replace('''  host-tests:''','''  abi-cross-check:
    name: ABI cross-check (kernel / app / helper / patch)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: python3 tools/abi_check.py

  host-tests:''')
open(p,'w',encoding='utf-8',newline='').write(s)

p=r'C:\Users\Lenovo\Desktop\kernelSU\XEChide\README.md'
s=open(p,encoding='utf-8').read()
s=s.replace('3. **Verify**: `tools/probe/` reproduces what a detector sees on your device.','3. **Verify**: `tools/probe.sh` reproduces what a detector sees on your device.')
open(p,'w',encoding='utf-8',newline='').write(s)
print('ci + readme updated')
