# config protocol

The manager, the root helper and both enforcement forms share one fixed-size
struct (`kernel/xechide.h`). The kernel form receives it through ioctls; the
module form reads the same bytes from a root-only file via its companion.

## Wire struct (packed, 32-bit fields)

```c
#define XECHIDE_ABI_MAGIC   0x58454348u  /* "XECH" */
#define XECHIDE_ABI_VERSION 1u
#define XECHIDE_MAX_RULES   64
#define XECHIDE_PKG_LEN     128
#define XECHIDE_UID_ALL     0xffffffffu

struct xechide_rule {
    unsigned int target_uid; /* who must not see it, or XECHIDE_UID_ALL */
    unsigned int hidden_uid; /* the app's uid, used for /proc and binder */
    unsigned int flags;      /* XECHIDE_RF_* */
    unsigned char pkg[128];  /* NUL-terminated package name */
};

struct xechide_config {
    unsigned int magic;
    unsigned int version;
    unsigned int n_rules;
    unsigned int flags;      /* XECHIDE_CF_* */
    struct xechide_rule rules[64];
};
```

Packed size: `4 x 4 + 64 x (3 x 4 + 128) = 16 + 64 x 140 = 8976` bytes,
little-endian, no padding (`__attribute__((packed))`). `xechide-ctl set` refuses
any stdin that is not exactly that size, and `tools/abi_check.py` cross-checks
the numbers in this document, the header, the Kotlin codec and the helper.

```
XECHIDE_RF_PATHS   0x1   hide the app's files (L0)
XECHIDE_RF_PROC    0x2   hide the app's processes (L0)
XECHIDE_RF_BINDER  0x4   filter PackageManager requests/replies (L1)

XECHIDE_CF_HIDE_SHELL 0x1  also hide from adb shell (uid 2000)
XECHIDE_CF_HIDE_SELF  0x2  hide /data/adb/xechide from non-root uids
```

`hidden_uid` is what the kernel uses for `/proc/<pid>` ownership and for the
`/proc` readdir filter; the package name is enough for the file layer and for
the parcel rewrite. A rule with `hidden_uid == 0` is rejected.

## ioctls

```
KSU_IOCTL_XECHIDE_GET   = _IOC(_IOC_READ,  'K', 0xE0, 0)   perm: only_root
KSU_IOCTL_XECHIDE_SET   = _IOC(_IOC_WRITE, 'K', 0xE1, 0)   perm: only_root
KSU_IOCTL_XECHIDE_STATS = _IOC(_IOC_READ,  'K', 0xE2, 0)   perm: only_root
```

`SET` replaces the whole configuration. Validation, in order:

1. `magic`/`version`;
2. `n_rules <= XECHIDE_MAX_RULES`;
3. every `pkg` is non-empty, NUL-terminated and `[A-Za-z0-9._]` only;
4. no rule targets uid 0 / 1000, no `hidden_uid` is 0 / 1000;
5. `flags &= XECHIDE_RF_ALL`, and a rule with no layers left is rejected.

Anything invalid returns `-EINVAL` and the live configuration is untouched: the
old block is only released after the new one has been validated *and*
published with `rcu_assign_pointer`.

`GET` returns the live configuration, or a zero-rule one when nothing has been
pushed yet. `STATS` returns `struct xechide_stats` (path/dirent/proc hits,
parcel rebuilt/rewritten/skipped counters, installed hooks).

## Module consumer

The module form cannot use the ioctls (they belong to the patched KernelSU
driver). It consumes the **same bytes** from a root-only file:

```
/data/adb/xechide/config   0600 root:root   exactly 8976 bytes
```

`xechide-ctl apply` reads one packed struct from stdin, writes it atomically
(`config.tmp` → `rename`) and then, if a patched driver exists, sends the same
bytes with `KSU_IOCTL_XECHIDE_SET`. `xechide-ctl status` prints key=value lines
(`module=`, `module_so=`, `config=`, `config_bytes=`, `kernel=`, `version=`) for
the manager; `file-get` dumps the file.

In each target process, the Zygisk module's `preAppSpecialize` connects to its
root companion (`zygiskd64`), which reads the file and returns it over the
companion socket. The app process never opens `/data/adb` itself — that is what
keeps the module free of new SELinux policy. A process with no config, or no
rule whose `target_uid`/`XECHIDE_UID_ALL` matches its uid, installs no hooks.

## Persistence (manager side)

The kernel deliberately keeps no configuration across reboots: a kernel-side
file parser is attack surface, and a stale list would hide the wrong package
after a reinstall. The helper is stateless too.

- The manager stores the rule list in its own private directory, encrypted with
  an **Android Keystore** AES-GCM key (`EncryptedSharedPreferences`-equivalent).
  Crypto is the platform's job; the helper never sees a key.
- At boot the manager's `BOOT_COMPLETED` receiver reads it, builds the struct
  and pipes it into `xechide-ctl apply` through `su`.
- Missing, locked or undecryptable → empty configuration → nothing is hidden.

The helper binary lives at `/data/adb/xechide/bin/xechide-ctl`; in the kernel
form it is hidden from all non-root uids by the kernel's self-hiding rule, and
the module form hides its own `/data/adb` paths in the file hooks.

## Boot

1. Kernel boots with `n_rules == 0`; the hooks are installed but match nothing.
2. `BOOT_COMPLETED` → the manager pushes the stored configuration with one
   `apply` (module file; kernel ioctl when its driver is present).
3. Until then nothing is hidden - by design: a failed hide is a bug report, a
   wrong deny is a bricked device.
