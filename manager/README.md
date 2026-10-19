# Manager app

A **standalone Android app** (`dev.xechide.manager`) — its own package id, its
own JNI, its own resources. It imports nothing from any KernelSU fork and does
not require `ksud`.

## What it does

1. Reads the KernelSU fd the same way the KernelSU manager does (open the
   driver, `KSU_IOCTL_XECHIDE_GET`) and shows the current rules.
2. Lets you pick, per rule:
   - **hidden packages** — the apps that must disappear;
   - **targets** — the apps that must not see them (multi-select, plus a
     "every non-root app" switch, plus `adb shell` as an explicit target);
   - optionally `deny exec` for the hidden APK.
3. Resolves each hidden package to its **real** paths
   (`ApplicationInfo.sourceDir`, `dataDir`, obb dir; the APK dir has a random
   suffix) — this is why resolution must happen in the manager and the kernel
   only receives prefixes.
4. Pushes the full config with `KSU_IOCTL_XECHIDE_SET` (one call, RCU swap) and
   writes the encrypted copy for persistence (see `../docs/config-protocol.md`).
5. A "verify" tab reproduces what a detector sees for the selected target
   (uses the same queries a detector would).

## Why not reuse any existing manager UI kit

The project must stay independent and auditable: the UI is plain Jetpack
Compose + Material 3, no theme engine, no custom design system. That also keeps
the attack surface small (no image loaders, no webviews, no remote configs).

## Data flow

```
manager ──(JNI ioctl)──> kernel xechide_config (RAM)
   │                        ▲
   └──(AES-256-GCM)──> /data/adb/xechide/cfg.bin
                          (0600, hidden by L2 from every non-root uid)
```

At boot the manager (or a tiny `xechide-boot` helper) re-pushes the config; until
it does, the kernel has **zero rules** and hides nothing (fail-open).
