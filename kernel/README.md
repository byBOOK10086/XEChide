# kernel/ — the module side

These five files are compiled into `kernelsu.ko` (or the built-in module) by
`patches/xechide-core.patch`. They are the whole enforcement path: no daemon,
no userspace process.

| File | Role |
|---|---|
| `xechide.h` | ABI (`struct xechide_config`) + internal API. Shared verbatim with `ctl/` and `manager/`. |
| `xechide_core.c` | Rule storage (RCU swap), uid gate, `/proc` ownership check, config validation, ioctls. |
| `xechide_match.c` | Pure path/readdir matching. Compiled into the host test suite as-is. |
| `xechide_parcel.c` | Pure binder parcel rewriting. Compiled into the host test suite as-is. |
| `xechide_lsm.c` | L0: four LSM entries + the `iterate_dir` readdir filter. |
| `xechide_binder.c` | L1: `binder_transaction` / `binder_thread_read` probes, thread marks. |

## Hook points, and why

**L0 (files).** There is no `inode_readdir` LSM hook, so two mechanisms are
used:

- `inode_permission`, `inode_getattr`, `file_permission`, `file_open` are
  replaced through `ksu_lsm_hook()`. The replacement runs first and calls the
  original SELinux handler for everything it does not deny, so SELinux
  enforcement is unchanged. Denied paths return `-ENOENT`, never `-EACCES`:
  an `EACCES` would tell a detector that the file exists.
- `iterate_dir` is a kretprobe. On entry the directory context's `actor` is
  swapped for our wrapper; the wrapper drops hidden names and forwards the
  rest to the original actor. Nothing in the filesystem is modified.

**L1 (PackageManager).** Two probes, no binder-internal structures:

- `binder_transaction` runs in the *caller's* context, so `current_uid()` is
  the target uid. When the parcel carries the `IPackageManager` interface
  token the thread is marked and package names in the request are renamed in
  place (same length, so the parcel cannot desynchronise).
- `binder_thread_read` also runs in the caller's context. If the thread is
  marked and the buffer holds `BR_REPLY`, the reply parcel is rewritten on a
  private copy: `List<PackageInfo>`/`String[]` elements are removed and the
  count fixed, anything else is renamed in place.

Every parcel decision is all-or-nothing: the container is walked and its
element count validated before a single byte is changed; if anything does not
add up the reply is sent unchanged.

## Failure policy

| Failure | Result |
|---|---|
| `selinux_*` symbol not found | that hook is skipped, the others still install |
| `iterate_dir` not in kallsyms | readdir filtering off |
| binder probes fail to register | L1 off; L0 still works |
| path cannot be resolved | the operation is allowed (fail-open) |
| parcel does not validate | reply is sent unmodified |
| config rejected | previous rules stay in force |

The module never denies anything for uid 0 or 1000, and never denies writes.

## Configuration

`KSU_IOCTL_XECHIDE_GET` / `SET` / `STATS`, `perm_check = only_root`, command
numbers `0xE0..0xE2`. The structure is in `xechide.h`; `XECHIDE_RF_*` selects
the layers per rule, `XECHIDE_CF_*` tunes the global behaviour.

Nothing is persisted by the kernel. The helper pushes the encrypted
configuration at boot (`ctl/README.md`), and an empty configuration means
"hide nothing".

## Debugging

```
dmesg | grep xechide      # hook install results, config updates
cat /proc/kallsyms | grep -E 'selinux_(inode_permission|file_open)'
```

Counters are available through `KSU_IOCTL_XECHIDE_STATS` (`ctl/README.md`).
