# XEChide — threat model & limits

## Who we defend against

| Adversary | Capability | XEChide answer |
|---|---|---|
| Ordinary detector app | `getInstalledPackages`, `queryIntent*`, direct file reads, `/proc` scan | L1 + L0 + L0(`/proc`) |
| Detector with shell (`adb shell`, Shizuku) | uid 2000, `pm list packages`, `ls /data/app` | the same rules apply to uid 2000 — add it as a target |
| Detector that inspects its own process | `maps`, ART headers, JNI tables | nothing to find: no injection |
| Root scanner (`su -c`, kernel module) | reads kernel memory, `/proc/kallsyms` | **out of scope — assume loss** |
| Forensic image of `/data` | offline | config is encrypted; the list is in RAM only while running |

## What is deliberately NOT hidden

1. Packages installed **after** the rule was applied — their install broadcast
   has already gone out. Apply first.
2. Notifications posted by the hidden app (system_server posts them).
3. `dumpsys`-style output. `dumpsys` passes a file descriptor and
   `system_server` writes the dump into it directly; that stream never becomes a
   binder parcel, so the L1 filter does not see it (this includes
   `dumpsys package` and `cmd activity`). Covering it would need a read-side
   content redactor, which is a different (and much heavier) mechanism.
4. `Play Integrity` verdicts and anything server-side.
5. Files the hidden app writes into shared storage (`/sdcard/Download/...`) —
   only `/data/media/*/Android/data/<pkg>` is hidden.
6. The hidden app's **network** activity (no hooking of sockets).

## Fail-safe rules (enforced in code review)

- No deny for uid 1000 (system), 0 (root), or the init domain.
- Reads only: never block writes, `chmod`, `unlink`.
- Unknown binder transaction → pass through, log once.
- Any bounds check failure → pass through.
- The kernel never allocates unbounded memory from userspace input.

## Detection surface that remains

- **Timing**: a filtered `getInstalledPackages` is slightly slower than an
  unfiltered one (parcel walk). Within noise for a 3 KB parcel, measured in
  `docs/verification.md`.
- **Inconsistency**: `getPackageInfo(hidden)` returns null while
  `getPackagesForUid(uid_of_hidden)` would also be filtered — but a detector can
  still call `getInstalledPackages` with flags not covered by the filter. Every
  added code is an arms race; the audit log in `dmesg` shows which codes the
  device actually uses.
- **Procfs**: `/proc/<hidden_pid>/` is hidden only for target uids; a target
  that reads `/proc` via `openat2`/`statx` on an unlisted path may still see the
  pid until the dentry cache is invalidated (`docs/verification.md` §3).
