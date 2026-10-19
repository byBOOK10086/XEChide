# ctl/ — the root helper

`xechide-ctl` is the only process that talks to the kernel. It is deliberately
tiny and stateless:

```
xechide-ctl set     < config.bin    # one struct xechide_config, applied atomically
xechide-ctl get     > config.bin    # read it back
xechide-ctl stats                    # kernel counters
xechide-ctl version                  # ABI/size sanity check
```

It must run as **root**:

1. it asks for a KernelSU driver descriptor with the legacy
   `reboot(KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &fd)` supercall (the
   syscall fails with `EINVAL`, that is expected - the descriptor is installed
   on the way out);
2. it issues `KSU_IOCTL_XECHIDE_SET/GET/STATS`, which the kernel gate at
   `only_root`.

There is no crypto, no file and no persistence here. The manager app owns the
configuration in its private directory (Android Keystore) and pipes the struct
into `set` over the `su` pipe. That keeps the trusted computing base of the
helper at "parse argv, read stdin, one ioctl".

## Build

- Android (NDK): `cmake -B build -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 && cmake --build build`
- Host sanity check (CI does this): `cc -std=c11 -Wall -Wextra -Werror -o xechide-ctl xechide-ctl.c`

## Where it lives on the device

The manager installs it as `/data/adb/xechide/bin/xechide-ctl`, mode `0755`,
owner root. That path is hidden from every non-root uid by the kernel's own
self-hiding rule (`XECHIDE_CF_HIDE_SELF`), so even the helper's existence is not
enumerable by an app.

## Failure modes

| Situation | Behaviour |
|---|---|
| no KernelSU driver | exit 1, nothing is touched |
| stdin not exactly one struct | exit 3/4, nothing is touched |
| kernel rejects the config | exit 5, previous configuration stays live |
| not root | the descriptor is never installed, exit 1 |
