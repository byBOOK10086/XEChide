#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# XEChide - installer hook (Magisk/KernelSU/APatch).
#
# Creates the private config directory and, on APatch/KernelPatch, installs the
# native .kpm into APatch's KPM store so the boot loader picks it up.

mkdir -p /data/adb/xechide 2>/dev/null
chmod 700 /data/adb/xechide 2>/dev/null
chown root:root /data/adb/xechide 2>/dev/null

MODDIR=${MODPATH:-${0%/*}}
if [ "$APATCH" = "true" ] || [ -d /data/adb/ap ]; then
    if [ -f "$MODDIR/xechide.kpm" ]; then
        mkdir -p /data/adb/ap/kpm/xechide 2>/dev/null
        cp -f "$MODDIR/xechide.kpm" /data/adb/ap/kpm/xechide/xechide.kpm 2>/dev/null
        chmod 600 /data/adb/ap/kpm/xechide/xechide.kpm 2>/dev/null
        chown root:root /data/adb/ap/kpm/xechide 2>/dev/null
        chown root:root /data/adb/ap/kpm/xechide/xechide.kpm 2>/dev/null
    fi
fi
