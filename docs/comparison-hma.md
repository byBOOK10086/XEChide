# HMA-oss vs XEChide — where the hiding actually lives

## The short version

HMA-oss is a **userspace app that hooks other apps' processes** (Xposed/LSPosed
or an in-process injection) and rewrites what the framework returns. XEChide is
a **kernel feature** of KernelSU: it changes nothing in any app's process and
filters the data before the app can see it.

## Layer by layer

| Question | HMA-oss | XEChide |
|---|---|---|
| How is a package hidden from `getInstalledPackages`? | Java hook on `PackageManagerService` (or the app's `ApplicationPackageManager`), returns a filtered list | kernel rewrites the `PackageManager` **binder reply** (L1) |
| How is the app's directory hidden? | usually not at all; some versions hide the launcher entry | LSM denies **target → /data/app/<pkg>** (L0) |
| What does the target's process contain? | the hook framework, the Xposed module, a modified ART method | **nothing** |
| Can the target detect it from inside? | yes: `maps`, ART headers, JNI table, `getDeclaredMethods` anomalies | no: the process is stock |
| Does it survive a framework update? | needs a new hook signature | only if the binder transaction numbers change (audit log tells you) |
| Where is the hide list? | SharedPreferences XML in the app's data dir | kernel RAM + AES-GCM file, 0600, path hidden from non-root |
| Root required? | no (Xposed/LSPosed or Shizuku) | yes (kernel) |
| Works on a locked device without root? | yes | no |

## Why "lower level" is not just marketing

Hiding in the target's process is **self-contradictory**: the hidden data and
the code that hides it live in the same address space, so any sufficiently
paranoid detector can find the hiding code. Kernel-side filtering has no such
address space: the target cannot inspect kernel memory (unless it has root, in
which case all bets are off — see `SECURITY.md`).

The price is that XEChide can only be used on a KernelSU device and it must
track AIDL transaction numbers per Android release. That trade-off is the whole
point: fewer devices, more certain hiding.

## What we did NOT take from anyone

No HMA-oss code, no LSPosed code, no Shizuku code was read into this project.
The comparisons above are behavioural, from public documentation. The kernel
work builds on KernelSU's own hooks (GPL-3.0), which is attributed in
`NOTICE.md`.
