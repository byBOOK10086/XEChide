#!/system/bin/sh
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
} > /data/adb/xechide/status 2>/dev/null
chmod 600 /data/adb/xechide/status 2>/dev/null
chown root:root /data/adb/xechide/status 2>/dev/null
