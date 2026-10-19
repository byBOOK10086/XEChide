# tools

Nothing here is required to use XEChide; these are the checks that keep the
claims honest.

```
probe/      what a detector can still see, reproduced on-device
patchcheck/ builds patches/xechide-core.patch and verifies it applies cleanly
```

`probe/` is the important one: every release should record its output on a real
device in `docs/verification.md`, so the "what is still visible" table in
SECURITY.md stays true.
