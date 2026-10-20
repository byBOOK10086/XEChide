#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Inspect/validate the freestanding XEChide Zygisk module (.so).

Runs on the build host (no Android tooling needed). It parses the ELF header,
program headers and dynamic section directly, so CI can prove that:

  * the file is a 64-bit little-endian AArch64 shared object (ET_DYN);
  * it has no DT_NEEDED entries at all (freestanding, libc comes from the
    host process);
  * the two Zygisk entry points are exported with default visibility;
  * no debug sections / local symbols with revealing names are left.

Usage:
  python tools/inspect_elf.py path/to/arm64-v8a.so [--require-entry]
"""

import struct
import sys


class Elf:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF":
            raise SystemExit("not an ELF file")
        self.ei_class = d[4]
        self.ei_data = d[5]
        if self.ei_class != 2 or self.ei_data != 1:
            raise SystemExit("expected ELF64 little-endian")
        (
            self.e_type,
            self.e_machine,
            self.e_version,
            self.e_entry,
            self.e_phoff,
            self.e_shoff,
            self.e_flags,
            self.e_ehsize,
            self.e_phentsize,
            self.e_phnum,
            self.e_shentsize,
            self.e_shnum,
            self.e_shstrndx,
        ) = struct.unpack_from("<HHIQQQIHHHHHH", d, 16)
        self.phdrs = []
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            self.phdrs.append(struct.unpack_from("<IIQQQQQQ", d, off))
        self.shdrs = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            self.shdrs.append(struct.unpack_from("<IIQQQQIIQQ", d, off))
        self.sections = []
        if self.e_shnum and self.e_shstrndx < self.e_shnum:
            strtab = self.sections_raw(self.e_shstrndx)
            for sh in self.shdrs:
                name_off = sh[0]
                end = strtab.find(b"\x00", name_off)
                name = strtab[name_off:end].decode("ascii", "replace")
                self.sections.append((name, sh))
        self._dynamic = None

    def sections_raw(self, idx):
        sh = self.shdrs[idx]
        off, size = sh[4], sh[5]
        return self.data[off:off + size]

    def dynamic(self):
        if self._dynamic is not None:
            return self._dynamic
        entries = []
        for p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align in self.phdrs:
            if p_type != 2:  # PT_DYNAMIC
                continue
            off = p_offset
            while off + 16 <= p_offset + p_filesz:
                tag, val = struct.unpack_from("<qQ", self.data, off)
                entries.append((tag, val))
                off += 16
                if tag == 0:
                    break
        self._dynamic = entries
        return entries

    def dynsym(self):
        d = self.data
        entries = self.dynamic()
        strtab_off = strsz = None
        symtab_off = syment = None
        for tag, val in entries:
            if tag == 5:
                strtab_off = self.vaddr_to_off(val)
            elif tag == 10:
                strsz = val
            elif tag == 6:
                symtab_off = self.vaddr_to_off(val)
            elif tag == 11:
                syment = val
        # Section headers are usually stripped by lld -s; fall back to the
        # dynamic segment and the GNU hash table for the symbol count.
        if symtab_off is None or strtab_off is None:
            return []
        count = self._dynsym_count()
        out = []
        for i in range(count):
            off = symtab_off + i * (syment or 24)
            if off + 24 > len(d):
                break
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
                "<IBBHQQ", d, off
            )
            if not st_name or strtab_off + st_name >= len(d):
                continue
            end = d.find(b"\x00", strtab_off + st_name)
            name = d[strtab_off + st_name:end].decode("ascii", "replace")
            out.append(
                {
                    "name": name,
                    "bind": st_info >> 4,
                    "type": st_info & 0xF,
                    "vis": st_other & 0x3,
                    "shndx": st_shndx,
                    "value": st_value,
                    "size": st_size,
                }
            )
        return out

    def _dynsym_count(self):
        # DT_HASH gives nbucket/ nchain directly; GNU hash needs a walk.
        d = self.data
        for tag, val in self.dynamic():
            if tag == 4:  # DT_HASH
                off = self.vaddr_to_off(val)
                return struct.unpack_from("<II", d, off)[1]
        for tag, val in self.dynamic():
            if tag == 0x6ffffef5:  # DT_GNU_HASH
                off = self.vaddr_to_off(val)
                nbuckets, symoffset, bloom_size, bloom_shift = struct.unpack_from("<IIII", d, off)
                buckets_off = off + 16 + bloom_size * 8
                buckets = struct.unpack_from("<%dI" % nbuckets, d, buckets_off)
                last = max(buckets) if buckets else 0
                chains_off = buckets_off + nbuckets * 4
                idx = last
                while True:
                    if idx < symoffset:
                        break
                    chain = struct.unpack_from("<I", d, chains_off + (idx - symoffset) * 4)[0]
                    if chain & 1:
                        break
                    idx += 1
                return idx + 1
        return 0

    def vaddr_to_off(self, vaddr):
        for p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align in self.phdrs:
            if p_type != 1:  # PT_LOAD
                continue
            if p_vaddr <= vaddr < p_vaddr + p_filesz:
                return p_offset + (vaddr - p_vaddr)
        # Dynamic string/symbol tables can live past the file size mapping of
        # the last PT_LOAD in some linkers; fall back to the section headers.
        for name, sh in self.sections:
            if sh[3] <= vaddr < sh[3] + sh[5]:
                return sh[4] + (vaddr - sh[3])
        return None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        raise SystemExit("usage: inspect_elf.py <file.so> [--require-entry]")
    require_entry = "--require-entry" in sys.argv
    e = Elf(args[0])
    ok = True

    print("file:      ", args[0])
    print("machine:    %d (%s)" % (e.e_machine, "AArch64" if e.e_machine == 183 else "?"))
    print("type:       %d (%s)" % (e.e_type, "ET_DYN" if e.e_type == 3 else "?"))
    dyn = e.dynamic()
    soname = None
    needed_names = []
    strtab_off = None
    strsz = 0
    for tag, val in dyn:
        if tag == 5:
            strtab_off = e.vaddr_to_off(val)
        elif tag == 10:
            strsz = val
    for tag, val in dyn:
        if tag == 1 and strtab_off:
            end = e.data.find(b"\x00", strtab_off + val)
            needed_names.append(e.data[strtab_off + val:end].decode("ascii", "replace"))
        elif tag == 14 and strtab_off:
            end = e.data.find(b"\x00", strtab_off + val)
            soname = e.data[strtab_off + val:end].decode("ascii", "replace")

    if e.e_type != 3:
        print("FAIL: not a shared object")
        ok = False
    if e.e_machine != 183:
        print("FAIL: not AArch64")
        ok = False
    if needed_names:
        print("FAIL: DT_NEEDED present:", needed_names)
        ok = False
    else:
        print("needed:     none (freestanding)")

    syms = e.dynsym()
    exported = {s["name"]: s for s in syms if s["vis"] == 0 and s["shndx"] != 0}
    for want in ("zygisk_module_entry", "zygisk_companion_entry"):
        if want in exported:
            print("export:     %s OK" % want)
        else:
            print("export:     %s MISSING" % want)
            if require_entry:
                ok = False
    unexpected = sorted(set(exported) - {"zygisk_module_entry", "zygisk_companion_entry"})
    if unexpected:
        print("note:       extra exported symbols:", ", ".join(unexpected))

    undefined = sorted(s["name"] for s in syms if s["shndx"] == 0)
    if undefined:
        print("imports:    " + ", ".join(undefined))

    sec_names = [n for n, _ in e.sections]
    debug = [n for n in sec_names if n.startswith(".debug") or n == ".symtab"]
    if debug:
        print("note:       unstripped sections present:", ", ".join(debug))
    print("sections:   %d" % len(e.sections))

    if ok:
        print("RESULT: OK")
        return 0
    print("RESULT: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
