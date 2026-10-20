# SPDX-License-Identifier: GPL-2.0
# Build the XEChide Zygisk module with Zig (no Android NDK required).
#
#   pwsh -File module\build.ps1              # build arm64-v8a.so
#   pwsh -File module\build.ps1 -Zip         # build and package the module zip
#
# Zygisk looks for zygisk/<abi>.so inside the module zip. We build a
# freestanding aarch64-linux-android shared object: no libc++/NDK dependency,
# libc symbols are resolved from the host app process at load time.

param(
    [string]$Zig = "",
    [string]$OutDir = "",
    [switch]$Zip
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if (-not $Zig) {
    $Zig = Join-Path (Split-Path -Parent $root) "_scratch\zig\zig-x86_64-windows-0.15.2\zig.exe"
}
if (-not (Test-Path $Zig)) {
    throw "zig not found at $Zig (pass -Zig <path>)"
}
if (-not $OutDir) {
    $OutDir = Join-Path $PSScriptRoot "zygisk"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$out = Join-Path $OutDir "arm64-v8a.so"
$sources = @(
    (Join-Path $PSScriptRoot "src\xechide_util.cpp"),
    (Join-Path $PSScriptRoot "src\xechide_binder.cpp"),
    (Join-Path $PSScriptRoot "src\xechide_file.cpp"),
    (Join-Path $PSScriptRoot "src\xechide_module.cpp"),
    (Join-Path $root "kernel\xechide_parcel.c"),
    (Join-Path $root "kernel\xechide_match.c")
)

$flags = @(
    "c++",
    "-target", "aarch64-linux-android",
    "-nostdlib", "-nostdlib++",
    "-fno-exceptions", "-fno-rtti", "-fno-builtin",
    "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-fPIC", "-shared", "-O2", "-fvisibility=hidden",
    "-DXECHIDE_HOST",
    "-Wall", "-Wextra", "-Werror",
    "-I", (Join-Path $PSScriptRoot "src"),
    "-I", (Join-Path $PSScriptRoot "compat"),
    "-I", (Join-Path $root "kernel"),
    "-Wl,--build-id=none",
    "-Wl,-s",
    "-o", $out
)

Write-Host "zig: $Zig"
Write-Host "out: $out"
& $Zig @flags @sources
if ($LASTEXITCODE -ne 0) {
    throw "zig exited with $LASTEXITCODE"
}
$size = (Get-Item $out).Length
Write-Host ("built {0} ({1} bytes)" -f $out, $size)

if ($Zip) {
    $version = "v1.0.0"
    foreach ($line in Get-Content (Join-Path $PSScriptRoot "module.prop")) {
        if ($line -match '^version=(.+)$') { $version = $Matches[1] }
    }
    $stage = Join-Path $PSScriptRoot "out"
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    $zipPath = Join-Path $stage ("xechide-module-{0}.zip" -f $version)
    python (Join-Path $PSScriptRoot "tools\pack_module.py") $PSScriptRoot $zipPath
    if ($LASTEXITCODE -ne 0) { throw "pack_module.py failed" }
    python (Join-Path $PSScriptRoot "tools\lint_module.py") $zipPath
    if ($LASTEXITCODE -ne 0) { throw "lint_module.py failed" }
    Write-Host ("packaged {0} ({1} bytes)" -f $zipPath, (Get-Item $zipPath).Length)
}
