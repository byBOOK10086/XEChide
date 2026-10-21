#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Build the native XEChide KPM against a pinned KernelPatch SDK.
#
#   ZIG=/path/to/zig bash kpm/build.sh
#   KP_SRC=/path/to/KernelPatch bash kpm/build.sh     # offline / already fetched
#
# A .kpm is a relocatable AArch64 ELF built with KernelPatch's public KPM
# headers; it is loaded by APatch's KPM engine (or any engine that provides the
# KernelPatch runtime). No kernel source tree and no Android NDK are needed.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
zig="${ZIG:-$(command -v zig || true)}"
if [ -z "$zig" ] || [ ! -x "$zig" ]; then
    echo "zig not found; set ZIG=/path/to/zig" >&2
    exit 1
fi

# Pinned KernelPatch commit: headers only, fetched at build time. The commit is
# the KPM loader/runtime ABI this module is built and validated against.
KP_COMMIT="052c73f93dbb84861218086e60a9962c48ff3f7f"
KP_SRC="${KP_SRC:-}"
if [ -z "$KP_SRC" ]; then
    cache="${XDG_CACHE_HOME:-$HOME/.cache}/xechide-kpm/KernelPatch-$KP_COMMIT"
    if [ ! -f "$cache/kernel/include/kpmodule.h" ]; then
        mkdir -p "$(dirname "$cache")"
        tarball="$(dirname "$cache")/KernelPatch-$KP_COMMIT.tar.gz"
        # -k is only here for hosts with a broken local CA store; the commit in
        # the URL is the integrity pin, and CI uses normal TLS.
        curl -fsSLk --retry 3 "https://codeload.github.com/bmax121/KernelPatch/tar.gz/$KP_COMMIT" -o "$tarball"
        rm -rf "$cache.tmp" "$cache"
        mkdir -p "$cache.tmp"
        tar -xzf "$tarball" -C "$cache.tmp" --strip-components=1
        mv "$cache.tmp" "$cache"
    fi
    KP_SRC="$cache"
fi
if [ ! -f "$KP_SRC/kernel/include/kpmodule.h" ]; then
    echo "KP_SRC does not look like KernelPatch: $KP_SRC" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
out="$here/out/xechide.kpm"
mkdir -p "$here/out"

common=(
    -target aarch64-freestanding
    -nostdlib -nostdinc
    -fno-pic -fno-PIC -fno-pie -fno-stack-protector
    -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-exceptions
    -fno-builtin -mno-outline-atomics -O2 -g0
    -Wall -Wextra
    -Wno-unused-parameter -Wno-cast-function-type-mismatch
    -I "$here/compat"
    -I "$KP_SRC/kernel/include"
    -I "$KP_SRC/kernel/patch/include"
    -I "$KP_SRC/kernel/linux/include"
    -I "$KP_SRC/kernel/linux/arch/arm64/include"
    -I "$KP_SRC/kernel/linux/tools/arch/arm64/include"
    -I "$root/kernel"
    -I "$here/src"
)

i=0
objs=()
for src in "$here/src/xechide_kpm.c" "$root/kernel/xechide_parcel.c" "$root/kernel/xechide_match.c"; do
    obj="$work/part$i.o"
    i=$((i + 1))
    "$zig" cc "${common[@]}" -c -o "$obj" "$src"
    objs+=("$obj")
done
"$zig" ld.lld -r -S -T "$here/xechide.lds" -o "$out" "${objs[@]}"
python3 "$here/tools/kpm_check.py" "$out"
printf 'built %s (%s bytes)\n' "$out" "$(wc -c < "$out")"
