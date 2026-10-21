#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Build and package the XEChide Zygisk module (Linux/macOS CI).
# Requires: zig 0.15.x and python3. No Android NDK is needed.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
zig="${ZIG:-$(command -v zig || true)}"
if [ -z "$zig" ] || [ ! -x "$zig" ]; then
    echo "zig not found; set ZIG=/path/to/zig" >&2
    exit 1
fi

out="$here/zygisk/arm64-v8a.so"
mkdir -p "$(dirname "$out")"

"$zig" c++ \
    -target aarch64-linux-android \
    -nostdlib -nostdlib++ \
    -fno-exceptions -fno-rtti -fno-builtin \
    -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -fPIC -shared -O2 -fvisibility=hidden \
    -DXECHIDE_HOST \
    -I "$here/src" -I "$here/compat" -I "$root/kernel" \
    -Wall -Wextra -Werror \
    -Wl,--build-id=none -Wl,-s \
    -o "$out" \
    "$here/src/xechide_util.cpp" \
    "$here/src/xechide_binder.cpp" \
    "$here/src/xechide_file.cpp" \
    "$here/src/xechide_module.cpp" \
    "$root/kernel/xechide_parcel.c" \
    "$root/kernel/xechide_match.c"

version="$(grep '^version=' "$here/module.prop" | head -n1 | cut -d= -f2)"
zip_path="$here/out/xechide-module-${version}.zip"
python3 "$here/tools/pack_module.py" "$here" "$zip_path"
python3 "$here/tools/inspect_elf.py" "$out" --require-entry
python3 "$here/tools/lint_module.py" "$zip_path"
