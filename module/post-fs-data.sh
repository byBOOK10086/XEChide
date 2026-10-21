#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# XEChide - create the private config directory before any app starts.
# Ownership/permissions are fixed up here because the companion process runs
# with root privileges but must never trust a world-writable directory.

mkdir -p /data/adb/xechide 2>/dev/null
chmod 700 /data/adb/xechide 2>/dev/null
chown root:root /data/adb/xechide 2>/dev/null

if [ ! -f /data/adb/xechide/config ]; then
    : > /data/adb/xechide/config
fi
chmod 600 /data/adb/xechide/config 2>/dev/null
chown root:root /data/adb/xechide/config 2>/dev/null

# APatch loads/controls KPMs from the APatch manager app's uid, which cannot
# traverse /data/adb. Keep a private 0600 mirror for KPM_INIT/ctl0 there.
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
