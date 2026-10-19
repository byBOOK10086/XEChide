# HMA-oss vs XEChide — where the hiding actually lives

## The short version

HMA-oss is a **userspace app that hooks other apps' processes** (Xposed/LSPosed
or an in-process injection) and rewrites what the framework returns. XEChide is
a **kernel feature** of KernelSU: it changes nothing in any app's process and
filters the data before the app can see it.

## Layer by layer

| Question | HMA-oss | XEChide |
|---|---|---|
| How is a package hidden from `getInstalledPackages`? | Java hook on `PackageManagerService` (or the app's `ApplicationPackageManager`), returns a filtered list | the kernel rewrites the **binder reply parcel** of the target's own call (L1) |
| How is a direct query hidden (`getPackageInfo`, `getApplicationInfo`)? | the same Java hook | the kernel renames the package inside the **request** parcel, so PMS itself answers "not found" (L1) |
| How is the app's directory hidden? | usually not at all; some versions hide the launcher entry | LSM denies **target → /data/app/<pkg>**, and the readdir filter drops the entry (L0) |
| How are process/task lists hidden? | usually out of scope for the module | the same parcel filter covers `IActivityManager`, `IActivityTaskManager`, `IUsageStatsManager`, `IAppOpsService`, including `pkg:remote` process names (L1) |
| How is the package database protected? | file is untouched | `/data/system/packages.list` and `packages.xml` are denied for target uids, so parsing the database cannot bypass the binder filter (L0) |
| What does the target's process contain? | the hook framework, the Xposed module, a modified ART method | **nothing** |
| Can the target detect it from inside? | yes: `maps`, ART headers, JNI table, `getDeclaredMethods` anomalies | no: the process is stock |
| Does it break on a framework update? | needs a new hook signature | no code table is involved: the selector is the `IPackageManager` interface token and the parcel shape |
| Where is the hide list? | SharedPreferences XML in the app's data dir | kernel RAM (RCU) + the manager's Keystore-encrypted store; the kernel never sees a file |
| Root required? | no (Xposed/LSPosed or Shizuku) | yes (kernel) |
| Works on a locked device without root? | yes | no |

## Why "lower level" is not just marketing

Hiding in the target's process is **self-contradictory**: the hidden data and
the code that hides it live in the same address space, so any sufficiently
paranoid detector can find the hiding code. Kernel-side filtering has no such
address space: the target cannot inspect kernel memory (unless it has root, in
which case all bets are off — see `SECURITY.md`).

The price is that XEChide only runs on a KernelSU device with its patch applied,
and that the reply rewriting has to stay conservative: the parcel is either
restructured after its element count validates, or renamed in place (same
length, layout untouched). A detector that resolves every listed package can
spot a renamed entry, which is documented in `SECURITY.md` rather than papered
over.

## What we did NOT take from anyone

No HMA-oss code, no LSPosed code, no Shizuku code was read into this project.
The comparisons above are behavioural, from public documentation. The kernel
work builds on KernelSU's own hooks (GPL-3.0), which is attributed in
`NOTICE.md`.
