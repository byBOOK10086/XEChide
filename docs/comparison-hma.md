# HMA-oss vs XEChide — where the hiding actually lives

## The short version

HMA-oss is a **Java Xposed module**: LSPosed injects it into the detector's
process and it hooks `PackageManager` methods. XEChide's module form is a
**native Zygisk module** with no LSPosed dependency: it hooks the PLT of
`libbinder.so` and `libjavacore.so` and filters the data at the binder and
filesystem boundary. XEChide's KPM form is a **native KernelPatch module** on
APatch, and the kernel form moves the same decisions into a patched kernel.

## Layer by layer

| Question | HMA-oss | XEChide module | XEChide KPM (APatch) | XEChide kernel |
|---|---|---|---|---|
| Dependencies | LSPosed (on Zygisk/Riru) | Zygisk only (Magisk, or Zygisk Next on KSU/APatch) | APatch/KernelPatch KPM runtime | patched KernelSU kernel |
| `getInstalledPackages` | Java hook on PMS/`ApplicationPackageManager` | binder reply parcel rewritten before Java | kernel binder reply rewritten | binder reply rewritten in kernel |
| `getPackageInfo(hidden)` | Java hook returns null | request/reply parcel names rewritten; hidden entries removed | request renamed, PMS itself answers not-found | request renamed, PMS itself answers not-found |
| Native binder callers | not covered (Java only) | covered: `libbinder` ioctl is hooked | covered by kernel hooks | covered |
| External-storage app dirs | not hidden | `Android/data|media|obb` entries hidden, paths answer `ENOENT` | syscall hooks hide paths/dirents (64-bit) | LSM denies the same paths |
| `maps` self-inspection | finds Xposed classes/ART changes | finds a hooked GOT and an anonymous image | finds nothing (no code in the process) | finds nothing |
| Direct `syscall(SYS_openat)` | can bypass Java hooks | bypasses PLT hooks | kernel syscall hooks catch it | kernel still enforces |
| Process/task/usage lists | partial (per-version Java hooks) | same 12-service parcel filter as the kernel | same 12-service parcel filter | same |
| Hide list at rest | SharedPreferences XML in the app data dir | `/data/adb/xechide/config`, 0600 root, plaintext | same file + APatch-private mirror (0600, APatch uid) | kernel RAM only |
| Root required | no (LSPosed/Shizuku) | root solution with Zygisk | APatch/KernelPatch | root solution with a custom kernel |
| SELinux policy change | none | none (companion socket for config) | none (forged `ENOENT`, no policy change) | none (deny added after SELinux allows) |

## "Lower level" is not marketing — but it depends on the form

Hiding inside the target process is **self-contradictory**: the hidden data and
the code hiding it share an address space, so a detector can inspect itself and
find the hook. HMA has that property; so does XEChide's module form (with fewer
framework fingerprints: no XposedBridge classes, no module class loader, no
LSPosed daemon, no Java method-table edits).

The kernel and KPM forms remove the contradiction: the detector's process is
stock, and the data is gone before it arrives. That is the form to choose if the
adversary is a native self-inspection checker.

All three XEChide forms accept the same configuration and the same manager, so
the choice is operational (custom kernel vs. Zygisk vs. APatch KPM), not a
rewrite.

## What we did NOT take from anyone

No HMA-oss code, no LSPosed code and no Shizuku code was read into this project.
The comparisons are behavioural, from public documentation. The kernel work
builds on KernelSU's own hooks (GPL-3.0), attributed in `NOTICE.md`. The Zygisk
ABI structs in `module/src/zygisk_api.h` were written against the published
interface documentation, not copied from a module implementation.
