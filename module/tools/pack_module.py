#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Deterministically package the XEChide Zygisk module zip.

The module root is the zip root: Magisk reads module.prop from there and
Zygisk Next looks for zygisk/<abi>.so. Entry names always use '/'.
"""

import os
import sys
import zipfile


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: pack_module.py <module-dir> <out.zip>")
    src = sys.argv[1]
    out = sys.argv[2]

    props = {}
    with open(os.path.join(src, "module.prop"), "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if "=" in line and not line.startswith("#"):
                key, value = line.split("=", 1)
                props[key] = value
    version = props.get("version", "unknown")

    files = [
        "module.prop",
        "customize.sh",
        "post-fs-data.sh",
        "service.sh",
        "uninstall.sh",
        "zygisk/arm64-v8a.so",
    ]
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in files:
            path = os.path.join(src, name)
            if not os.path.isfile(path):
                raise SystemExit("missing " + path)
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o755 if name.endswith(".sh") else 0o644
            info.external_attr = mode << 16
            with open(path, "rb") as f:
                z.writestr(info, f.read())
    print("packaged %s (%d bytes, %s)" % (out, os.path.getsize(out), version))


if __name__ == "__main__":
    main()
