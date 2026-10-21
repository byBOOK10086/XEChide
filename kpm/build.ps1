# SPDX-License-Identifier: GPL-3.0-only
# Build the native XEChide KPM on Windows (Zig's clang + lld, no NDK).
#
#   powershell -ExecutionPolicy Bypass -File kpm\build.ps1
#   powershell -ExecutionPolicy Bypass -File kpm\build.ps1 -KpSrc <KernelPatch-dir>
#
param(
    [string]$Zig = "",
    [string]$KpSrc = ""
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = Split-Path -Parent $here
if (-not $Zig) {
    $Zig = Join-Path (Split-Path -Parent $root) "_scratch\zig\zig-x86_64-windows-0.15.2\zig.exe"
}
if (-not (Test-Path $Zig)) { throw "zig not found at $Zig (pass -Zig <path>)" }

$kpCommit = "052c73f93dbb84861218086e60a9962c48ff3f7f"
if (-not $KpSrc) {
    $local = Join-Path (Split-Path -Parent $root) "_scratch\kpatch_src"
    if (Test-Path (Join-Path $local "kernel\include\kpmodule.h")) {
        $KpSrc = $local
    } else {
        $cacheRoot = Join-Path $env:LOCALAPPDATA "xechide-kpm"
        $KpSrc = Join-Path $cacheRoot "KernelPatch-$kpCommit"
        if (-not (Test-Path (Join-Path $KpSrc "kernel\include\kpmodule.h"))) {
            New-Item -ItemType Directory -Force $cacheRoot | Out-Null
            $tar = Join-Path $cacheRoot "KernelPatch-$kpCommit.tar.gz"
            if (-not (Test-Path $tar)) {
                curl.exe -k -L -sS -o $tar "https://codeload.github.com/bmax121/KernelPatch/tar.gz/$kpCommit"
            }
            $tmp = "$KpSrc.tmp"
            if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
            New-Item -ItemType Directory -Force $tmp | Out-Null
            tar -xzf $tar -C $tmp --strip-components=1
            if (Test-Path $KpSrc) { Remove-Item $KpSrc -Recurse -Force }
            Move-Item $tmp $KpSrc
        }
    }
}
if (-not (Test-Path (Join-Path $KpSrc "kernel\include\kpmodule.h"))) {
    throw "KP_SRC does not look like KernelPatch: $KpSrc"
}

$build = Join-Path $here "build"
New-Item -ItemType Directory -Force $build | Out-Null
New-Item -ItemType Directory -Force (Join-Path $here "out") | Out-Null
$out = Join-Path $here "out\xechide.kpm"
$kp = ($KpSrc -replace '\\', '/')
$xe = ($root -replace '\\', '/')

$common = @(
    "cc", "-target", "aarch64-freestanding",
    "-nostdlib", "-nostdinc",
    "-fno-pic", "-fno-PIC", "-fno-pie", "-fno-stack-protector",
    "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-fno-exceptions",
    "-fno-builtin", "-mno-outline-atomics", "-O2", "-g0",
    "-Wall", "-Wextra",
    "-Wno-unused-parameter", "-Wno-cast-function-type-mismatch",
    "-I", "$xe/kpm/compat",
    "-I", "$kp/kernel/include",
    "-I", "$kp/kernel/patch/include",
    "-I", "$kp/kernel/linux/include",
    "-I", "$kp/kernel/linux/arch/arm64/include",
    "-I", "$kp/kernel/linux/tools/arch/arm64/include",
    "-I", "$xe/kernel",
    "-I", "$xe/kpm/src"
)
$sources = @(
    "$here/src/xechide_kpm.c",
    "$root/kernel/xechide_parcel.c",
    "$root/kernel/xechide_match.c"
)
$objs = @()
$i = 0
foreach ($src in $sources) {
    $obj = Join-Path $build ("part$i.o")
    $i++
    & $Zig @common "-c" "-o" $obj $src
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $src" }
    $objs += $obj
}
& $Zig ld.lld -r -S -T (Join-Path $here "xechide.lds") -o $out @objs
if ($LASTEXITCODE -ne 0) { throw "link failed" }
python (Join-Path $here "tools\kpm_check.py") $out
if ($LASTEXITCODE -ne 0) { throw "kpm_check failed" }
Get-Item $out | Select-Object FullName, Length
