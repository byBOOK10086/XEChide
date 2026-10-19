# config protocol

The manager and the kernel share one fixed-size struct; there is no filesystem
protocol.

## Wire struct (little-endian, packed by the compiler)

```c
struct xechide_cmd {
    __u32 magic;        /* 0x58454348 "XECH" */
    __u32 version;      /* 1 */
    __u32 n_rules;      /* <= 128 */
    __u32 reserved;
    struct {            /* xechide_rule */
        __u32 target_uid;   /* 0xFFFFFFFF = every non-root uid */
        __u8  pkg[128];     /* NUL-terminated package name */
        __u8  flags;        /* bit0 all-targets, bit1 deny-exec */
        __u8  n_prefix;
        __u8  pad[2];
        char  prefix[3][256]; /* resolved by the manager, not the kernel */
    } rules[128];
};
```

IoCTLs (claimed ABI, see `../patches/README.md`):

```
KSU_IOCTL_XECHIDE_GET  = _IOC(_IOC_READ,  'K', 0xE0, 0)   /* perm: only_manager */
KSU_IOCTL_XECHIDE_SET  = _IOC(_IOC_WRITE, 'K', 0xE1, 0)   /* perm: only_manager */
```

`SET` validates: magic/version, `n_rules <= 128`, package name charset
`[A-Za-z0-9._]`, target uid not 0/1000. Anything else → `-EINVAL`, and the live
config is left untouched (the swap is the last step).

## Persistence

- File: `/data/adb/xechide/cfg.bin`, mode 0600, owner root.
- Content: AES-256-GCM over the struct; nonce prepended; key = 32 random bytes
  generated once at install time and stored in the manager's private dir.
- Who writes it: only the manager. Who reads it: the manager, at boot or after a
  config change from another profile.
- Missing or undecryptable → zero rules (nothing hidden). Never guess.

## Boot

1. Kernel boots with `n_rules == 0`.
2. Manager (or its boot receiver) decrypts `cfg.bin` and issues one `SET`.
3. Until then nothing is hidden — by design: a failed hide is a bug report, a
   wrong deny is a bricked device.
