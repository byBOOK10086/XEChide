#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# XEChide - publish a tiny status file for the manager app.
# The manager reads it through its root helper; normal apps cannot read
# /data/adb at all, so this file is not a metadata leak.

MODDIR=${0%/*}
VERSION=$(grep '^version=' "$MODDIR/module.prop" 2>/dev/null | head -n 1 | cut -d= -f2)
[ -z "$VERSION" ] && VERSION=unknown

{
    echo "version=$VERSION"
    echo "zygisk=1"
    if [ -s /data/adb/xechide/config ]; then
        echo "config=1"
    else
        echo "config=0"
    fi
    if [ -f /data/adb/ap/kpm/xechide/xechide.kpm ]; then
        echo "kpm=1"
    else
        echo "kpm=0"
    fi
} > /data/adb/xechide/status 2>/dev/null
chmod 600 /data/adb/xechide/status 2>/dev/null
chown root:root /data/adb/xechide/status 2>/dev/null

# Keep the APatch-app-uid config mirror (same reason as post-fs-data.sh).
ap_dir=/data/data/me.bmax.apatch
if [ -d "$ap_dir" ] && [ -s /data/adb/xechide/config ]; then
    mkdir -p "$ap_dir/files" 2>/dev/null
    cp -f /data/adb/xechide/config "$ap_dir/files/xechide.cfg" 2>/dev/null
    owner=$(stat -c %u "$ap_dir" 2>/dev/null)
    group=$(stat -c %g "$ap_dir" 2>/dev/null)
    [ -n "$owner" ] || owner=$(/data/adb/ap/bin/busybox stat -c %u "$ap_dir" 2>/dev/null)
    [ -n "$group" ] || group=$(/data/adb/ap/bin/busybox stat -c %g "$ap_dir" 2>/dev/null)
    if [ -n "$owner" ] && [ -n "$group" ]; then
        chown "$owner:$group" "$ap_dir/files" "$ap_dir/files/xechide.cfg" 2>/dev/null
    fi
    chmod 600 "$ap_dir/files/xechide.cfg" 2>/dev/null
fi
