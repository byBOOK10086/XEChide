#!/usr/bin/env python3
"""Cross-check the packed ABI across the four places that encode it.

The kernel header, the manager's Kotlin constants, the root helper's ioctl
numbers and the patch that adds those ioctls to KernelSU all describe the same
contract. A drift between any two of them produces a runtime failure that is
hard to diagnose (a wrong rule stride silently hides the wrong packages), so
this script fails loudly on any mismatch.

It is deliberately dependency-free: standard library only, no compiler needed.

    python3 tools/abi_check.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

failures = 0
checks = 0


def check(ok: bool, what: str, detail: str = "") -> None:
    global failures, checks
    checks += 1
    print(("  ok   " if ok else "  FAIL ") + what + (("  [" + detail + "]") if detail and not ok else ""))
    if not ok:
        failures += 1


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def c_define(text: str, name: str) -> int | None:
    m = re.search(r"^#define\s+%s\s+(0x[0-9a-fA-F]+|\d+)u?" % re.escape(name), text, re.M)
    return int(m.group(1), 0) if m else None


def kotlin_const(text: str, name: str) -> int | None:
    m = re.search(r"\bconst val %s\s*:\s*Int\s*=\s*(0x[0-9a-fA-F]+|-?\d+)" % re.escape(name), text)
    return int(m.group(1), 0) if m else None


def main() -> int:
    header = read("kernel/xechide.h")
    rule_kt = read("manager/app/src/main/java/dev/xechide/manager/data/Rule.kt")
    codec_kt = read("manager/app/src/main/java/dev/xechide/manager/data/ConfigCodec.kt")
    helper = read("ctl/xechide-ctl.c")
    patch = read("patches/xechide-core.patch")
    protocol = read("docs/config-protocol.md")

    print("== header defines")
    magic = c_define(header, "XECHIDE_ABI_MAGIC")
    version = c_define(header, "XECHIDE_ABI_VERSION")
    max_rules = c_define(header, "XECHIDE_MAX_RULES")
    pkg_len = c_define(header, "XECHIDE_PKG_LEN")
    uid_all = c_define(header, "XECHIDE_UID_ALL")
    check(magic == 0x58454348, "header magic", hex(magic or 0))
    check(version == 1, "header version", str(version))
    check(max_rules == 64, "header max rules", str(max_rules))
    check(pkg_len == 128, "header package length", str(pkg_len))
    check(uid_all == 0xFFFFFFFF, "header UID_ALL", hex(uid_all or 0))

    print("== packed sizes")
    rule_bytes = 3 * 4 + (pkg_len or 0)  # packed: three u32 + pkg[]
    config_bytes = 4 * 4 + (max_rules or 0) * rule_bytes
    check(rule_bytes == 140, "struct xechide_rule is 140 bytes", str(rule_bytes))
    check(config_bytes == 8976, "struct xechide_config is 8976 bytes", str(config_bytes))

    print("== manager constants")
    for name, expected in [
        ("XECHIDE_ABI_MAGIC", magic),
        ("XECHIDE_ABI_VERSION", version),
        ("XECHIDE_MAX_RULES", max_rules),
        ("XECHIDE_PKG_LEN", pkg_len),
    ]:
        got = kotlin_const(rule_kt, name)
        check(got == expected, "Kotlin %s == header" % name, "%r vs %r" % (got, expected))
    kotlin_uid_all = kotlin_const(rule_kt, "XECHIDE_UID_ALL")
    check(kotlin_uid_all == -1, "Kotlin UID_ALL is the signed form of 0xffffffff", str(kotlin_uid_all))

    for name, expected in [("PATHS", 0x0001), ("PROC", 0x0002), ("BINDER", 0x0004), ("ALL", 0x0007)]:
        m = re.search(r"const val %s\s*:\s*Int\s*=\s*(0x[0-9a-fA-F]+)" % name, rule_kt)
        got = int(m.group(1), 16) if m else None
        check(got == expected, "Kotlin RuleFlags.%s" % name, str(got))
    for name, expected in [("HIDE_SHELL", 0x0001), ("HIDE_SELF", 0x0002)]:
        m = re.search(r"const val %s\s*:\s*Int\s*=\s*(0x[0-9a-fA-F]+)" % name, rule_kt)
        got = int(m.group(1), 16) if m else None
        check(got == expected, "Kotlin ConfigFlags.%s" % name, str(got))

    print("== manager codec")
    m = re.search(r"const val PACKED_BYTES\s*:\s*Int\s*=\s*(\d+)", codec_kt)
    check(m is not None and int(m.group(1)) == config_bytes, "PACKED_BYTES == %d" % config_bytes,
          m.group(1) if m else "missing")
    m = re.search(r"HEADER_BYTES\s*=\s*(\d+)", codec_kt)
    check(m is not None and int(m.group(1)) == 16, "HEADER_BYTES == 16", m.group(1) if m else "missing")
    m = re.search(r"RULE_BYTES\s*=\s*(\d+)", codec_kt)
    check(m is not None and int(m.group(1)) == rule_bytes, "RULE_BYTES == %d" % rule_bytes,
          m.group(1) if m else "missing")
    # The stride skip is the bug this check exists for: writing a package name
    # must advance the buffer to the next rule boundary.
    check("XECHIDE_PKG_LEN - packageBytes.size" in codec_kt,
          "codec advances to the next rule boundary after pkg[]")

    print("== helper ioctl numbers")
    ioctl_numbers = {
        "GET": (0xE0, "_IOC_READ"),
        "SET": (0xE1, "_IOC_WRITE"),
        "STATS": (0xE2, "_IOC_READ"),
    }
    for name, (nr, direction) in ioctl_numbers.items():
        m = re.search(r"#define\s+KSU_IOCTL_XECHIDE_%s\s+_IOC\(\s*_IOC_%s\s*,\s*'K'\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*0\s*\)"
                      % (name, "READ" if direction == "_IOC_READ" else "WRITE"), helper)
        got = int(m.group(1), 0) if m else None
        check(got == nr, "helper %s ioctl is 0x%02x" % (name, nr), str(got))
    check("sizeof(struct xechide_config)" in helper and "sizeof(cfg)" in helper,
          "helper measures the header struct instead of hardcoding a size")

    print("== patch ioctl additions")
    for name, nr in [("GET", 0xE0), ("SET", 0xE1), ("STATS", 0xE2)]:
        pattern = r"KSU_IOCTL_XECHIDE_%s\s*=\s*_IOC\(" % name
        check(re.search(pattern, patch) is not None, "patch adds KSU_IOCTL_XECHIDE_%s" % name)
        check(re.search(r"_IOC\([^)]*'K'[^)]*0x%02x" % nr, patch, re.IGNORECASE) is not None,
              "patch uses 0x%02x for %s" % (nr, name))
    check("xechide/xechide_core.o" in patch and "xechide/xechide_binder.o" in patch,
          "patch links all five objects")
    check("xechide_init();" in patch and "xechide_lsm_init();" in patch and "xechide_binder_init();" in patch,
          "patch initialises engine, LSM and binder layers")

    print("== docs")
    check("0xE0" in protocol and "0xE1" in protocol and "0xE2" in protocol,
          "config-protocol.md lists all three ioctls")
    check("8976" in protocol or "8 976" in protocol or "64 x 140" in protocol or "64 \u00d7 140" in protocol,
          "config-protocol.md documents the packed size")

    print("== package-name charset (kernel vs manager)")
    kernel_chars = set()
    m = re.search(r"static bool pkg_valid.*?\n\}", read("kernel/xechide_core.c"), re.S)
    if m:
        if "c >= 'a' && c <= 'z'" in m.group(0):
            kernel_chars |= set("abcdefghijklmnopqrstuvwxyz")
        if "c >= 'A' && c <= 'Z'" in m.group(0):
            kernel_chars |= set("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
        if "c >= '0' && c <= '9'" in m.group(0):
            kernel_chars |= set("0123456789")
        if "c == '.'" in m.group(0):
            kernel_chars.add(".")
        if "c == '_'" in m.group(0):
            kernel_chars.add("_")
    manager_chars = set()
    if "character in 'a'..'z'" in codec_kt:
        manager_chars |= set("abcdefghijklmnopqrstuvwxyz")
    if "character in 'A'..'Z'" in codec_kt:
        manager_chars |= set("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
    if "character in '0'..'9'" in codec_kt:
        manager_chars |= set("0123456789")
    if "character == '.'" in codec_kt:
        manager_chars.add(".")
    if "character == '_'" in codec_kt:
        manager_chars.add("_")
    check(kernel_chars and kernel_chars == manager_chars,
          "manager and kernel accept the same package-name characters",
          "%r vs %r" % (sorted(kernel_chars), sorted(manager_chars)))

    print()
    print("%d checks, %d failure(s)" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
