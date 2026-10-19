# tools/

Two kinds of things live here:

- `probe.sh` — a read-only script that prints what a detector still sees on a
  running device, for the surfaces XEChide claims to cover: `pm list packages`,
  the package/data directories, external storage, and the helper's own path.
  Run it once as `shell` and once as root; root is out of scope and the script
  says so.
- `patchcheck.sh` — verifies that `patches/xechide-core.patch` still applies to
  the pinned stock KernelSU commit without downloading the whole history. CI
  runs the same check.
- `abi_check.py` — cross-checks the packed ABI everywhere it is encoded (the
  kernel header, the manager's Kotlin constants and codec stride, the helper's
  ioctl numbers, the patch, the protocol doc, and the package-name charset).
  This is the check that would have caught the rule-stride bug by itself.

```
sh tools/probe.sh com.example.hidden
sh tools/patchcheck.sh
```

`probe.sh` needs no root for the interesting part; only the helper stats line
uses `su`.

A full detector exists as an Android app, not in this directory: the app-level
binder path is exactly what the manager's stats screen counts
(`parcel_rebuilt` / `parcel_rewritten`), so a device test is "start the detector
app, see zero leaks, see the counters move". `docs/verification.md` has the
checklist.
