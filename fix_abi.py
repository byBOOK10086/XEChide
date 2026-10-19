p=r'C:\Users\Lenovo\Desktop\kernelSU\XEChide\tools\abi_check.py'
s=open(p,encoding='utf-8').read()
s=s.replace("""        check(re.search(r\"_IOC\\([^)]*'K'[^)]*0x%02x\" % nr, patch) is not None,
              \"patch uses 0x%02x for %s\" % (nr, name))""","""        check(re.search(r\"_IOC\\([^)]*'K'[^)]*0x%02x\" % nr, patch, re.IGNORECASE) is not None,
              \"patch uses 0x%02x for %s\" % (nr, name))""")
open(p,'w',encoding='utf-8',newline='').write(s)

p=r'C:\Users\Lenovo\Desktop\kernelSU\XEChide\docs\config-protocol.md'
s=open(p,encoding='utf-8').read()
s=s.replace('''struct xechide_config {
    unsigned int magic;
    unsigned int version;
    unsigned int n_rules;
    unsigned int flags;      /* XECHIDE_CF_* */
    struct xechide_rule rules[64];
};
```''','''struct xechide_config {
    unsigned int magic;
    unsigned int version;
    unsigned int n_rules;
    unsigned int flags;      /* XECHIDE_CF_* */
    struct xechide_rule rules[64];
};
```

Packed size: `4 x 4 + 64 x (3 x 4 + 128) = 16 + 64 x 140 = 8976` bytes,
little-endian, no padding (`__attribute__((packed))`). `xechide-ctl set` refuses
any stdin that is not exactly that size, and `tools/abi_check.py` cross-checks
the numbers in this document, the header, the Kotlin codec and the helper.''')
open(p,'w',encoding='utf-8',newline='').write(s)
print('fixed')
