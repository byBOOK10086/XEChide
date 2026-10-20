#!/system/bin/sh
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
