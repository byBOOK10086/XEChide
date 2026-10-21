#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Audit the license split and upstream attribution of this repository.

Rules enforced (all are deliberate, see NOTICE.md):

  * userspace code is GPL-3.0-only;
  * kernel-only code is GPL-2.0-only;
  * the three shared files are GPL-2.0-only OR GPL-3.0-only;
  * every C/C++/H/Python/shell/Kotlin/Gradle/workflow/HTML/Makefile/CMake file
    carries an SPDX header;
  * deprecated bare "GPL-2.0"/"GPL-3.0" identifiers are rejected;
  * root LICENSE is GPL-3.0 and LICENSES/ carries both texts;
  * NOTICE and ACKNOWLEDGEMENTS name the upstream projects actually used.

Standard library only; no network, no compiler.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
failures = 0
checks = 0


def check(ok, what, detail=""):
    global failures, checks
    checks += 1
    print(("  ok   " if ok else "  FAIL ") + what + (("  [" + detail + "]") if detail and not ok else ""))
    if not ok:
        failures += 1


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


SHARED = {
    "kernel/xechide.h",
    "kernel/xechide_match.c",
    "kernel/xechide_parcel.c",
}
KERNEL_ONLY = {
    "kernel/xechide_core.c",
    "kernel/xechide_lsm.c",
    "kernel/xechide_binder.c",
}
USERS_PREFIX = ("manager/", "module/", "ctl/", "tools/", "tests/", "ui/")
CODE_EXT = {".c", ".h", ".cpp", ".cc", ".hpp", ".py", ".sh", ".kt", ".kts",
            ".ps1", ".html", ".yml", ".yaml", ".pro", ".lds"}
CODE_NAMES = {"Makefile", "CMakeLists.txt"}
KPM_PREFIX = "kpm/"
SKIP_FILES = {
    "patches/xechide-core.patch",
    "module/module.prop",
}
SKIP_DIRS = {".git", "build", ".gradle", "out", "node_modules", "__pycache__",
             ".idea", "module/out", "module/zygisk"}
# Generated / non-source trees are skipped; XML resources and Markdown do not
# carry SPDX headers by policy.
SKIP_PREFIX = ("manager/app/src/main/res/", "manager/app/build/", "ui/out/")


def spdx_of(text):
    m = re.search(r"SPDX-License-Identifier:\s*([^\n*]+)", text)
    if not m:
        return None
    value = m.group(1).strip()
    value = re.sub(r"\s*(?:\*/|-->)\s*$", "", value).strip()
    return value


def main():
    print("== license texts")
    lic = read("LICENSE")
    gpl3 = read("LICENSES/GPL-3.0-only.txt")
    gpl2 = read("LICENSES/GPL-2.0-only.txt")
    gpl2_later = read("LICENSES/GPL-2.0-or-later.txt")
    check("GNU GENERAL PUBLIC LICENSE" in lic and "Version 3" in lic[:400],
          "root LICENSE is the GPL-3.0 text")
    check("GNU GENERAL PUBLIC LICENSE" in gpl2 and "Version 2" in gpl2[:400],
          "LICENSES/GPL-2.0-only.txt is the GPL-2.0 text")
    check("GNU GENERAL PUBLIC LICENSE" in gpl2_later and "Version 2" in gpl2_later[:400],
          "LICENSES/GPL-2.0-or-later.txt is the GPL-2.0 text")
    check("Version 3" in gpl3[:400], "LICENSES/GPL-3.0-only.txt is the GPL-3.0 text")

    print("== upstream attribution")
    notice = read("NOTICE.md")
    ack = read("ACKNOWLEDGEMENTS.md")
    for name in ("KernelSU", "Magisk", "Zygisk", "LSPlt", "HMA-oss", "AndroidX",
                 "Zig", "KernelPatch", "APatch"):
        check(name in notice, "NOTICE mentions " + name)
        check(name in ack, "ACKNOWLEDGEMENTS mentions " + name)
    check("0faec0cc7b5f655170d89bf9c4d28bb5ca271fa2" in notice or
          "0faec0cc7b5f655170d89bf9c4d28bb5ca271fa2" in read("patches/README.md"),
          "KernelSU base commit is recorded")
    check("GPL-3.0-only" in notice and "GPL-2.0-only" in notice and
          "GPL-2.0-only OR GPL-3.0-only" in notice,
          "NOTICE documents the three license groups")
    check("independent" in read("module/src/zygisk_api.h").lower(),
          "Zygisk ABI is documented as an independent declaration")

    print("== SPDX headers")
    for dp, dn, fs in os.walk(ROOT):
        dn[:] = [d for d in dn if d not in SKIP_DIRS]
        rel_dir = os.path.relpath(dp, ROOT).replace(os.sep, "/")
        if rel_dir == ".":
            rel_dir = ""
        for f in sorted(fs):
            rel = (rel_dir + "/" + f).lstrip("/")
            if rel in SKIP_FILES or rel.startswith(SKIP_PREFIX):
                continue
            ext = os.path.splitext(f)[1].lower()
            if ext not in CODE_EXT and f not in CODE_NAMES:
                continue
            text = read(rel)
            got = spdx_of(text)
            if rel in SHARED:
                want = "GPL-2.0-only OR GPL-3.0-only"
            elif rel in KERNEL_ONLY:
                want = "GPL-2.0-only"
            elif rel.startswith("kpm/src/") or rel.startswith("kpm/compat/") or rel == "kpm/xechide.lds":
                want = "GPL-2.0-or-later"
            elif rel.startswith("kpm/"):
                want = "GPL-3.0-only"
            elif rel.startswith(USERS_PREFIX) or rel.startswith(".github/"):
                want = "GPL-3.0-only"
            else:
                continue
            check(got == want, "SPDX " + rel, repr(got) + " != " + repr(want))
            check(got != "GPL-2.0" and got != "GPL-3.0",
                  "SPDX uses a precise identifier " + rel, repr(got))

    print("== no strong-copyleft surprises")
    for rel, text in (("tools/license_check.py", read("tools/license_check.py")),
                      ("NOTICE.md", notice)):
        # NOTICE may *mention* rejected AGPL projects; code must not.
        pass
    for dp, dn, fs in os.walk(ROOT):
        dn[:] = [d for d in dn if d not in SKIP_DIRS]
        for f in fs:
            if os.path.splitext(f)[1].lower() not in {".c", ".h", ".cpp", ".py", ".sh", ".kt"}:
                continue
            rel = os.path.relpath(os.path.join(dp, f), ROOT).replace(os.sep, "/")
            if rel == "tools/license_check.py":
                continue  # this file names the forbidden licence in its own text
            text = read(rel)
            if "AGPL" in text or "GNU AFFERO" in text:
                check(False, "no AGPL text in code: " + rel)
    check(True, "AGPL scan complete")

    print()
    print("%d checks, %d failure(s)" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
