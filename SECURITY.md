# XEChide — threat model & limits

XEChide ships two enforcement forms with the same configuration: the **module**
form (native Zygisk, userspace) and the **kernel** form (patched KernelSU).
Their detection surfaces are different; this document says which is which.

## Who we defend against

| Adversary | Capability | Module form | KPM form | Kernel form |
|---|---|---|---|---|
| Ordinary detector app | `getInstalledPackages`, `queryIntent*`, direct file access | binder parcels filtered before Java; `libjavacore` paths answer `ENOENT` | kernel binder/syscall hooks, no in-process code | kernel filters replies and path lookups |
| Detector that inspects **its own** process | `maps`, ELF/dynsym scan, GOT check, memory scan | **can find** the injected image / a patched GOT entry | **nothing to find** (kernel hooks) | **nothing to find** |
| Detector with shell (`adb shell`, Shizuku) | uid 2000, `pm list packages`, `ls /data/app` | not injected into shell, so not covered | uid 2000 is a normal syscall caller; rules can target it | uid 2000 is just another target |
| Root scanner (`su -c`) | kernel memory, `/proc/kallsyms`, `/data/adb` | out of scope; root sees the module/config | out of scope; root sees inline hooks/kallsyms | out of scope; root sees the kernel hooks |
| Forensic image of `/data` | offline | config is root-only, plaintext at rest (see below) | config is root-only, plaintext at rest | config is kernel RAM only |

The module form is a **userspace injection** by definition. It reduces what a
Java-level detector sees, and it covers native binder and file access that HMA's
Java hooks miss, but it cannot win the "inspect my own address space" game. If
that is the threat you care about, use the kernel form.

## What is deliberately NOT hidden

1. Packages installed **after** the config was applied (module processes fetch
   the config at start, kernel rules are static until reapplied).
2. The hidden app's **notifications** (posted by system_server) and its network
   traffic.
3. `dumpsys` output: it writes to a file descriptor, not a binder reply. Neither
   form covers it.
4. Play Integrity verdicts and anything server-side.
5. Files the hidden app writes into shared storage outside
   `Android/{data,media,obb}`.
6. In the **module** form: direct syscalls (`syscall(SYS_openat)`) and file
   access from native libraries loaded **after** Zygisk's pre-specialization
   commit. The hooks live in `libjavacore.so` / `libbinder.so`; app-owned native
   libraries are not patched.
7. In the **module** form: the config file is plaintext at rest
   (`/data/adb/xechide/config`, 0600 root). The kernel form keeps rules in kernel
   memory only. Root can read either.

## Fail-safe rules (enforced in code review)

- Kernel form: no deny for uid 0 / 1000; reads only; unknown binder transaction
  or failed bounds check → pass through.
- Module form: no config or no rule for this uid → no hooks are installed at
  all; a missing symbol, a failed commit, an unparsable parcel or a missing
  backup pointer → the original function runs unchanged; a failed file write
  never deletes the previous config.
- Parcel rewriting stays conservative: restructure only when the element count
  and boundaries validate, otherwise rename in place with an identical length.
- KPM form: the same conservative parcel rule plus syscall hooks that only turn
  an allowed lookup into `-ENOENT`; a missing runtime symbol disables the
  dependent hooks instead of faking success.

## Module-form detection surface that remains

- **`maps`**: the module image is loaded from a `memfd` named `jit-cache`
  (Magisk's own naming, not ours). We filter reads of `/proc/self/maps`,
  `/proc/self/smaps` and `map_files`, but a detector issuing raw syscalls or
  reading its own memory can still find an anonymous executable mapping.
- **GOT**: `libbinder`'s `ioctl` and `libjavacore`'s file functions have replaced
  GOT entries. Reading those tables inside the detector's own process exposes
  the hook; there is no way around this in userspace.
- **Memory**: the hook code and the per-process rule names are readable by the
  process itself.
- **Direct syscalls**: `syscall(SYS_openat, ...)` bypasses the PLT hooks.
- **Zygisk/root itself**: `zygiskd64`, the native-bridge property, Magisk or
  Zygisk Next presence — these belong to the root solution, not to XEChide.

Mitigations that are implemented: config never enters the app's filesystem view;
`/data/adb` paths are hidden; the two public Zygisk export names are erased from
`.dynstr` after load; `maps`/`smaps` are filtered; no logs or debug strings are
emitted in release; the module is loaded by Zygisk from memfd, so no
`/data/adb/modules/...` path appears in the target's mappings.

## KPM-form detection surface that remains

The KPM runs in the kernel, so there is no injected image, GOT entry or module
mapping in any app process. What remains is root/kernel-level:

- **kallsyms / inline hooks**: `hook_wrap` patches the prologue of
  `binder_transaction`/`binder_thread_read` (and the syscall wrappers). A root
  scanner can inspect kernel text, kallsyms and Module KPM metadata. Ordinary
  apps cannot read kernel memory or `/proc/kallsyms`.
- **KPM metadata**: `/data/adb/ap/kpm/xechide/xechide.kpm` and the KPM list are
  root-readable only; the metadata is intentionally honest (`name=xechide`,
  `license=GPL v2`).
- **Config mirror**: the helper copies the packed config to
  `/data/data/me.bmax.apatch/files/xechide.cfg` (0600, APatch uid) so APatch's
  app-uid KPM loader/control can read it. The APatch manager (a root manager)
  and root can read it; ordinary apps cannot cross the app-data boundary.
- **APatch/KernelPatch itself**: the root solution is visible to root detection;
  XEChide does not try to hide its host.
- 32-bit path syscalls are not hooked (binder hooks are ABI-independent).
- `dumpsys`, notifications, Play Integrity and post-install packages are the
  same exclusions as the other forms.

SELinux: the KPM form does not change SELinux policy. Syscall hooks return a
forged `-ENOENT` before the syscall reaches SELinux, so no `avc` denial is
generated; binder filtering changes parcels only after SELinux’s binder checks.

## SELinux

- **Module form**: no policy changes. The app process never reads
  `/data/adb/xechide`; the root companion reads it and passes bytes over the
  Zygisk companion socket (created while the process still runs with zygote
  privileges). File hooks return **forged `ENOENT`**, so they do not generate
  `avc: denied` records; they only make an already-denied path quieter.
- **Kernel form**: the LSM wrappers call the original SELinux handler first and
  only add `-ENOENT` after SELinux has allowed the operation. SELinux is never
  weakened (that would be a privilege-escalation bug), the L2 self-hide rule is
  a VFS-level `ENOENT` and does not alter xattrs or policy. Because the deny is
  added after the SELinux decision, no extra `avc` noise is produced. The real
  risk in this form is kernel-version coupling (`security_hook_heads`, RCU
  lifetime) and binder parcel correctness, not SELinux policy.

## Root-level traces

- Module form: `/data/adb/modules/xechide/`, `/data/adb/xechide/`, the memfd
  mapping, the companion socket; ordinary apps cannot read `/data/adb`
  (`adb_data_file`, root/shell only), so these are root/forensic traces.
- Kernel form: function names in `/proc/kallsyms` on `KALLSYMS_ALL` kernels,
  replaced LSM slots and kprobe entries (debugfs), all root-only. Symbol-name
  scrubbing remains on the hardening list; no kernel log lines are emitted in
  release builds.
