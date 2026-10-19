# assets — root-helper staging area

This directory is packaged verbatim into the APK. One file is expected here at
release time:

```
xechide-ctl-arm64
```

It is **not checked into this repository**. CI, or whoever builds a release
APK, must build the helper from `ctl/` with the Android NDK and copy it to
`manager/app/src/main/assets/xechide-ctl-arm64` before running
`:app:assembleRelease`. The binary has no extension in the APK; the manager
copies it to `/data/adb/xechide/bin/xechide-ctl` and verifies `test -x`.

## Producing the asset

From the repository root, with an NDK installed. The only targeted ABI is
`arm64-v8a`; no on-device verification has been performed in this repository:

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

On a multi-config generator (for example Visual Studio on Windows), the output
is under `build-ctl/Release/`. Copy the built ELF binary, not the host build
from the CI sanity check.

Check that the APK really contains the file:

```sh
unzip -l manager/app/build/outputs/apk/release/app-release.apk \
  | grep 'assets/xechide-ctl-arm64'
```

`README.md` itself is also packaged; that is harmless. Do not add a text file
under the binary's name — Android would install it as a script that cannot
talk to the kernel.

The helper source is GPL-2.0 like the kernel part; see `NOTICE.md` at the
repository root.
