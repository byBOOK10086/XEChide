# tests

Two things can be checked without a phone:

## 1. Host tests (`tests/host`)

`kernel/xechide_match.c` and `kernel/xechide_parcel.c` are compiled from the
same sources the module uses, with a tiny `-DXECHIDE_HOST` shim instead of the
kernel headers:

```
make -C tests/host
```

The suite builds real binder parcels (`List<PackageInfo>` in both String16 and
String8 encodings, `String[]` arrays, request parcels) and asserts that

- hidden elements are removed, the container count is updated, the other
  elements survive byte for byte;
- a container that does not validate is never restructured - only renamed;
- a parcel without a hidden name is left completely untouched;
- path/dirent matching accepts the real Android layouts (`/data/app/~~hash/`,
  `/data/user/<n>`, `/data/media/<n>/Android/data`, `/sdcard/Android/data`)
  and rejects look-alike names.

## 2. Patch check

`patches/xechide-core.patch` applies cleanly to stock KernelSU
`0faec0cc7b5f655170d89bf9c4d28bb5ca271fa2`:

```
git clone --filter=blob:none https://github.com/tiann/KernelSU.git /tmp/ksu
cd /tmp/ksu
git checkout 0faec0cc7b5f655170d89bf9c4d28bb5ca271fa2
git apply --check <repo>/patches/xechide-core.patch
```

CI runs both jobs on every push (`.github/workflows/ci.yml`).

## What is **not** covered

The LSM/kprobe glue (`xechide_lsm.c`, `xechide_binder.c`, `xechide_core.c`) can
only be compiled against a real kernel tree. Building the patched KernelSU
module for a target KMI is the acceptance test; see `patches/README.md`.
