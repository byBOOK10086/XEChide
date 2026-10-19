p=r'C:\Users\Lenovo\Desktop\kernelSU\XEChide\.github\workflows\ci.yml'
s=open(p,encoding='utf-8').read()
s=s.replace('''  host-tests:
    name: host tests (matching + parcel)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Build and run
        run: make -C tests/host
''','''  host-tests:
    name: host tests (matching + parcel + ASan fuzz)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Build, run and fuzz
        run: make -C tests/host
''')
open(p,'w',encoding='utf-8',newline='').write(s)

p=r'C:\Users\Lenovo\Desktop\kernelSU\XEChide\tests\README.md'
s=open(p,encoding='utf-8').read()
s=s.replace('''## 2. Patch check''','''Then `make -C tests/host fuzz` builds the same parser with
`-fsanitize=address,undefined` and feeds it 200k deterministic random parcels
(plus every boundary shape). `make -C tests/host` runs both.

## 2. ABI cross-check

```
python3 tools/abi_check.py
```

37 assertions over the places the packed ABI is encoded: header defines, packed
sizes (140/8976), Kotlin constants and codec stride, helper ioctl numbers, the
patch's ioctl additions, the protocol doc, and the package-name charset shared
by kernel and manager.

## 3. Patch check''')
open(p,'w',encoding='utf-8',newline='').write(s)
print('updated')
