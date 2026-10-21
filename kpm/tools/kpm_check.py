#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Validate a XEChide .kpm against the KernelPatch module format.

The KPM loader is strict about the ELF kind, metadata, lifecycle sections and
resolvable symbols, so this checks those statically (it cannot load the module
without a matching kernel). Build with kpm/build.sh; run directly on an
existing .kpm:

    python3 kpm/tools/kpm_check.py kpm/out/xechide.kpm

The loader resolves undefined symbols through KernelPatch's exported symbol
table, not arbitrary kallsyms. The blocklist below is exactly the set of
`kf_*` variables the KPM SDK declares but does not export; importing one of
them makes the module fail with "unknown symbol".
"""

import re
import struct
import sys

SHF_ALLOC = 0x2
SHT_NOBITS = 8
SHT_SYMTAB = 2
REQUIRED_SECTIONS = {".kpm.info", ".kpm.init", ".kpm.ctl0", ".kpm.exit", ".text", ".symtab"}
KNOWN_NON_EXPORTED = {
    "kf_filp_open",
    "kf_kernel_read",
    "kf_kernel_write",
    "kf_filp_close",
    "kf_iterate_dir",
}


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            raise SystemExit("not an ELF64 little-endian file")
        self.e_type, self.e_machine = struct.unpack_from("<HH", d, 16)
        (self.e_shoff,) = struct.unpack_from("<Q", d, 40)
        (self.e_shentsize,) = struct.unpack_from("<H", d, 58)
        (self.e_shnum,) = struct.unpack_from("<H", d, 60)
        (self.e_shstrndx,) = struct.unpack_from("<H", d, 62)
        self.sections = {}
        self.order = []
        for i in range(self.e_shnum):
            sh = self._sh(i)
            name = self._sname(sh[0])
            self.sections[name] = sh
            self.order.append(name)

    def _sh(self, i):
        return struct.unpack_from("<IIQQQQIIQQ", self.data, self.e_shoff + i * self.e_shentsize)

    def _sname(self, off):
        shstr = self._sh(self.e_shstrndx)
        base = shstr[4]
        end = self.data.find(b"\x00", base + off)
        return self.data[base + off:end].decode("ascii", "replace")

    def section_data(self, name):
        sh = self.sections[name]
        return self.data[sh[4]:sh[4] + sh[5]]

    def symtab(self):
        sym = self.sections[".symtab"]
        st = self.sections[".strtab"]
        strings = self.data[st[4]:st[4] + st[5]]
        und, defined = [], []
        for i in range(1, sym[5] // 24):
            off = sym[4] + i * 24
            name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", self.data, off)
            end = strings.find(b"\x00", name)
            s = strings[name:end].decode("ascii", "replace")
            if shndx == 0:
                und.append(s)
            else:
                defined.append(s)
        return und, defined

    def rela_types(self):
        seen = {}
        for name in self.order:
            if not name.startswith(".rela"):
                continue
            sh = self.sections[name]
            for i in range(sh[5] // 24):
                _, info, _ = struct.unpack_from("<QQq", self.data, sh[4] + i * 24)
                t = info & 0xFFFFFFFF
                seen[t] = seen.get(t, 0) + 1
        return seen


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: kpm_check.py <file.kpm>")
    path = sys.argv[1]
    e = Elf(path)
    ok = True

    def check(cond, what, detail=""):
        nonlocal ok
        print(("  ok   " if cond else "  FAIL ") + what + (("  [" + detail + "]") if detail and not cond else ""))
        if not cond:
            ok = False

    print("file:   ", path)
    check(e.e_type == 1, "ELF type is ET_REL (relocatable)", str(e.e_type))
    check(e.e_machine == 183, "machine is AArch64", str(e.e_machine))

    for name in sorted(REQUIRED_SECTIONS):
        check(name in e.sections, "section " + name)
    for name in e.order:
        check(not name.startswith(".debug") and name != ".eh_frame",
              "no debug section " + name)

    info = e.section_data(".kpm.info") if ".kpm.info" in e.sections else b""
    fields = {}
    for item in info.split(b"\x00"):
        if b"=" in item:
            key, value = item.split(b"=", 1)
            fields[key.decode("ascii", "replace")] = value.decode("utf-8", "replace")
    print("info:   ", ", ".join("%s=%s" % (k, v) for k, v in fields.items()))
    check(fields.get("name") == "xechide", "metadata name is xechide")
    for key in ("version", "license", "author", "description"):
        check(bool(fields.get(key)), "metadata has " + key)
    check("GPL" in fields.get("license", ""), "metadata license is GPL")

    init = e.sections.get(".kpm.init")
    exit_ = e.sections.get(".kpm.exit")
    ctl0 = e.sections.get(".kpm.ctl0")
    for name, sh in ((".kpm.init", init), (".kpm.exit", exit_), (".kpm.ctl0", ctl0)):
        check(sh is not None and sh[5] == 8, name + " contains one function pointer")
        check((".rela" + name) in e.sections, name + " has a RELA relocation")

    bss = e.sections.get(".bss")
    check(bss is not None, "has .bss")
    if bss:
        check(bss[5] < 8 * 1024 * 1024, "bss below 8 MiB", str(bss[5]))

    und, defined = e.symtab()
    print("imports:", ", ".join(sorted(set(und))) or "(none)")
    bad = sorted(set(und) & KNOWN_NON_EXPORTED)
    check(not bad, "no imports of non-exported KPM SDK variables", ",".join(bad))
    check("kallsyms_lookup_name" in und or "kallsyms_lookup_name" in defined,
          "module can resolve kernel symbols")
    print("defined:", len(defined), "sections:", len(e.sections),
          "relocation types:", dict(sorted(e.rela_types().items())))

    print("RESULT: " + ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
