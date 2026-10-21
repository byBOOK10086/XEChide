#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# XEChide - what a detector can still see.
#
# Run on the device, once as the app-shell user and once as root:
#   adb push tools/probe.sh /data/local/tmp/probe.sh
#   adb shell sh /data/local/tmp/probe.sh <hidden.package.name>
#   adb shell su -c 'sh /data/local/tmp/probe.sh <hidden.package.name>'
#
# The script only reads; it never changes anything.

PKG="$1"
if [ -z "$PKG" ]; then
    echo "usage: probe.sh <hidden.package.name>" >&2
    exit 2
fi

say() { echo; echo "== $*"; }

say "identity"
id 2>/dev/null || echo "uid=$(id -u)"
echo "uid=$(id -u) euid=$(id -u)"

say "PackageManager: pm list packages"
pm list packages 2>/dev/null | grep -F "$PKG" && echo "  ^ LEAK: visible to pm" || echo "  not listed (ok)"

say "PackageManager: getPackagesForUid"
# 10000 is the first app uid; the hidden app's uid is printed by the manager.
pm list packages -U 2>/dev/null | grep -F "$PKG" || echo "  no entry (ok)"

say "files: /data/app"
ls -la /data/app 2>&1 | grep -F "$PKG" && echo "  ^ LEAK" || echo "  not visible or no permission (ok)"

say "files: /data/data"
ls -la /data/data 2>&1 | grep -F "$PKG" && echo "  ^ LEAK" || echo "  not visible or no permission (ok)"

say "files: external storage"
ls -la /sdcard/Android/data 2>&1 | grep -F "$PKG" && echo "  ^ LEAK" || echo "  not visible (ok)"

say "files: obb"
ls -la /sdcard/Android/obb 2>&1 | grep -F "$PKG" && echo "  ^ LEAK" || echo "  not visible (ok)"

say "self-hiding: /data/adb/xechide"
ls -la /data/adb/xechide 2>&1 | head -n 5

say "binder: direct parcel sizes"
# A full detector would call IPackageManager through service; this only proves
# the shell path. The manager's own stats screen shows parcel counters.

say "kernel counters"
su -c '/data/adb/xechide/bin/xechide-ctl stats' 2>/dev/null || echo "  helper not installed or not root"

say "done"
echo "Remember: run this once as the detector's uid (e.g. shell) and once as root."
echo "Root is out of scope by design - XEChide does not hide from root."
