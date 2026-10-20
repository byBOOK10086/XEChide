#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Validate a packaged XEChide module zip.

Checks that the zip can be flashed by Magisk / KernelSU + Zygisk Next:

  * module.prop is at the root and has the required fields;
  * the scripts are at the root (no top-level wrapper directory);
  * entry names use forward slashes and never escape the archive;
  * zygisk/arm64-v8a.so is a freestanding AArch64 shared object that exports
    the two Zygisk entry points.

Usage: python module/tools/lint_module.py module/out/xechide-module-vX.zip
"""

import hashlib
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from inspect_elf import Elf  # noqa: E402

REQUIRED = {
    "module.prop",
    "post-fs-data.sh",
    "service.sh",
    "uninstall.sh",
    "customize.sh",
    "zygisk/arm64-v8a.so",
}

PROP_KEYS = ("id", "name", "version", "versionCode", "author", "description")


def fail(message):
    print("FAIL: " + message)
    return False


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: lint_module.py <module.zip>")
    path = sys.argv[1]
    ok = True
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        print("file:      ", path)
        print("entries:   ", len(names))
        print("sha256:    ", hashlib.sha256(open(path, "rb").read()).hexdigest())

        for name in names:
            if "\\" in name:
                ok = fail("backslash in entry name: %r" % name)
            if name.startswith("/") or ".." in name.split("/"):
                ok = fail("unsafe entry: %r" % name)

        missing = sorted(REQUIRED - set(names))
        if missing:
            ok = fail("missing entries: " + ", ".join(missing))
        else:
            print("layout:     OK")

        props = z.read("module.prop").decode("utf-8", "replace")
        values = {}
        for line in props.splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
        for key in PROP_KEYS:
            if not values.get(key):
                ok = fail("module.prop missing %s" % key)
        if values.get("id") != "xechide":
            ok = fail("module.prop id is not xechide: %r" % values.get("id"))
        try:
            int(values.get("versionCode", ""))
        except ValueError:
            ok = fail("module.prop versionCode is not an integer")
        if ok:
            print("module.prop: %s %s" % (values.get("version"), values.get("versionCode")))

        with z.open("zygisk/arm64-v8a.so") as so:
            data = so.read()
        elf_path = path + ".so"
        with open(elf_path, "wb") as tmp:
            tmp.write(data)
        try:
            elf = Elf(elf_path)
            if elf.e_type != 3 or elf.e_machine != 183:
                ok = fail("zygisk .so is not an AArch64 ET_DYN")
            needed = [tag for tag, _ in elf.dynamic() if tag == 1]
            if needed:
                ok = fail("zygisk .so has DT_NEEDED entries")
            exported = {
                s["name"] for s in elf.dynsym() if s["vis"] == 0 and s["shndx"] != 0
            }
            for want in ("zygisk_module_entry", "zygisk_companion_entry"):
                if want not in exported:
                    ok = fail("missing export " + want)
            print("zygisk .so: %d bytes, %d exported symbol(s)" % (len(data), len(exported)))
        finally:
            os.unlink(elf_path)

    print("RESULT: " + ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
