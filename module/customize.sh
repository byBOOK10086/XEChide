#!/system/bin/sh
# XEChide - installer hook (Magisk/KernelSU). Nothing is patched; just create
# the private config directory so the first boot does not race the companion.

mkdir -p /data/adb/xechide 2>/dev/null
chmod 700 /data/adb/xechide 2>/dev/null
chown root:root /data/adb/xechide 2>/dev/null
