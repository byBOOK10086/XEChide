# XEChide manager

Standalone Android manager for XEChide. Package `dev.xechide.manager`, Kotlin
2.0, Jetpack Compose + Material 3, minSdk 30, compileSdk/targetSdk 35. It is
independent of KernelSU's own manager and imports no code from any KernelSU
fork.

> **The app is not buildable as a release manager without the
> `app/src/main/assets/xechide-ctl-arm64` asset.** Gradle can compile and package
> the UI without it, but that APK cannot install the root helper and is not a
> usable XEChide manager. The missing-asset path exists so a debug build fails
> with an explicit message instead of a mysterious `127` from the shell. A
> release build must bundle the helper.

The app has not been run on a physical device in this repository. The kernel
side is host-tested; the manager and root helper are still unverified end to
end. Do not treat any screen as proof that the kernel patch is active.

## What it does

1. Enumerates installed apps (label, package name, uid, icon).
2. Lets the user choose:
   - hidden apps;
   - target apps, or "all non-root apps";
   - hiding from uid 2000, and self-path hiding.
3. Serializes a 8976-byte, little-endian `struct xechide_config` and pipes it
   to `su -c '/data/adb/xechide/bin/xechide-ctl set'`.
4. Encrypts the same selection as JSON (AES-256-GCM, Android Keystore) in the
   app's private SharedPreferences.
5. Re-applies the saved selection from a `BOOT_COMPLETED` receiver.
6. Installs/updates the helper from APK assets and verifies it with `test -x`.
7. Shows the output of `xechide-ctl stats`.

There is no JNI, no `libsu`, no `security-crypto`, and no daemon. Root commands
use `ProcessBuilder("su", "-c", ...)` from `Dispatchers.IO`, with a hard
timeout and stdout/stderr drained concurrently.

## Build requirements

The versions are pinned in `build.gradle.kts` / `app/build.gradle.kts`:

| Item | Version |
|---|---|
| Android Gradle Plugin | 8.7.3 |
| Gradle | 8.9 or newer |
| Kotlin / Compose compiler plugin | 2.0.21 |
| Compose BOM | 2024.09.03 |
| JDK | 17 |
| Android SDK | API 35 (`compileSdk = 35`) |
| NDK (only for the helper) | any current r26/r27; the helper is plain C |

A Gradle wrapper is intentionally not committed in this source tree. Open the
`manager/` directory in Android Studio (Ladybug or newer) and let it use a
Gradle 8.9 installation, or install Gradle 8.9 and run:

```sh
cd manager
gradle :app:assembleDebug
```

Install/dependencies resolve from `google()` and `mavenCentral()`; this source
was written without network access, so nothing in the tree depends on a local
Maven mirror.

### Produce and bundle the helper (required)

From the repository root:

```sh
cmake -S ctl -B build-ctl \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-30 \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-ctl --target xechide-ctl

cp build-ctl/xechide-ctl manager/app/src/main/assets/xechide-ctl-arm64
chmod 0755 manager/app/src/main/assets/xechide-ctl-arm64
```

Then build the APK. `app/src/main/assets/README.md` has the same instructions
plus the APK check. The asset must be an `arm64-v8a` Android ELF; the host
binary produced by the CI sanity check will not run on the phone.

Without the asset the **Install/update helper** button reports:

> The APK does not contain assets/xechide-ctl-arm64. A release APK bundles that
> binary. Build it with the NDK from ctl/ and place it in app/src/main/assets/
> as described in manager/README.md, then rebuild.

(That is the English wording; the default locale shows the Chinese equivalent.)

## Using it

1. Flash a KernelSU kernel that includes the XEChide patch and boot it.
2. Install the manager APK. Grant its `su` request when prompted.
3. Tap **安装/更新 helper** first. It copies the asset to
   `/data/adb/xechide/bin/xechide-ctl` (0755, root) and checks `test -x`.
4. Pick hidden apps and targets, apply, and confirm the result card. The
   `su`/helper stderr is shown on failure.
5. Reboot if you want to test the `BOOT_COMPLETED` re-apply path.

The kernel starts each boot with zero rules and hides nothing until the helper
receives `set`. This fail-open behaviour is intentional.

## Design notes

- `data/ConfigCodec.kt` is the only place that knows the packed ABI. It writes
  the header magic `0x58454348`, version 1, `n_rules`, global flags, then up to
  64 rules of 140 bytes (`3 × u32` + `pkg[128]`). The buffer is zero-filled, so
  unused rules and unused package-name bytes are zero.
- `data/SecureStore.kt` uses a non-exportable Android Keystore AES key. Only the
  IV and ciphertext are in SharedPreferences. Undecryptable state fails open.
- The last-apply metadata (success, exit code, stdout/stderr) is stored without
  encryption because it contains no rule list; the hide list itself is only in
  the encrypted config.
- `root/RootShell.kt` refuses to run on the main thread. `XeChideBridge` and
  `HelperInstaller` wrap it in `Dispatchers.IO`.
- `ui/App.kt` is a plain state-based screen switch; no Navigation dependency.

## Screenshots

_Placeholder — this project does not claim on-device verification yet._

| Home | App picker | Kernel stats |
|---|---|---|
| _screenshot pending_ | _screenshot pending_ | _screenshot pending_ |

## Limitations / things to know

- **No on-device verification.** The kernel patch has host-side tests only and
  the manager has not been flashed or exercised on a real phone. The docs say
  "not verified" instead of pretending otherwise.
- **ARM64 only in practice.** The build looks for `assets/xechide-ctl-arm64`.
  There is no x86_64 asset or ABI-adaptive packaging.
- **Helper status is shallow.** `test -x` proves only that the file exists and
  is executable. It does not prove that the kernel patch is loaded. The stats
  screen is the honest check: without the patched kernel the helper exits with
  an error.
- **The rules count is the intended config**, not a GET from the kernel. The
  manager does not reconcile its saved selection with kernel memory.
- **Per-layer flags are not exposed in the UI.** All generated rules use
  `PATHS | PROC | BINDER` (`0x7`). The model persists the field for a future
  per-layer UI.
- **uid 0 and uid 1000 are excluded** from both pickers, matching kernel
  validation; the manager also excludes itself.
- **Background root may be denied.** Some root managers do not grant `su` to a
  BOOT_COMPLETED receiver. In that case the boot re-apply fails and the status
  card shows the stored error after the app is opened.
- **No automatic re-apply after install/uninstall** of a package, and no
  watcher. Apply manually after changing the app set.
- **QUERY_ALL_PACKAGES is required** to enumerate all installed apps. That is
  normal for a sideloaded root tool but is restricted on Google Play.
- **Fail-open on crypto failure.** If the Keystore key is invalidated or app
  data is cleared, the saved config cannot be read and nothing is hidden until
  the user configures it again.
- **Root sees everything.** This is a hiding tool for non-root observation; a
  detector with root can read kernel memory and is explicitly out of scope.

## License

The project is GPL-3.0; the kernel part and helper source are GPL-2.0-only like
the kernel they build into. See the repository root's `LICENSE` (or the project
notice) and `NOTICE.md`.
