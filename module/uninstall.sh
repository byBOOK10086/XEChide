#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# XEChide - remove private module state and the APatch KPM copy on uninstall.
rm -rf /data/adb/xechide
rm -rf /data/adb/ap/kpm/xechide
