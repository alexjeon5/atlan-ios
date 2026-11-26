#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds an arm64-v8a-only APK of one app on the Android host, without Gradle: the native library with CMake (the
# NDK's toolchain, or host clang + the NDK's sysroot where the NDK has no compiler for this host), resources and
# manifest with aapt, Java with javac + d8, then zipalign and a debug-key signature.
#
#   platform/android/build_apk.sh --app adapters/<app>/android [--out DIR] [--asset FILE]... [--release-keystore KS]
#
# --app      folder with AndroidManifest.xml, res/, java/ and the app's CMake part (see adapters/atlan/android)
# --out      build folder (default build-android); the APK is DIR/<label>-arm64-v8a.apk
# --asset    a file copied into the APK's assets/ (for example a local input.cfg or native.sig); repeatable
# Environment: ANDROID_HOME (platforms/android-NN, build-tools/*), ANDROID_NDK_HOME (default: newest ANDROID_HOME/ndk/*)
# and optional overrides AAPT, D8_JAR, APKSIGNER_JAR, ZIPALIGN, ANDROID_PLATFORM_JAR. The debug key is
# ~/.android/debug.keystore (created when missing). The APK is for local installation only.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
app="" out="$root/build-android" assets=()
while [ $# -gt 0 ]; do
    case "$1" in
        --app) app="$(cd "$2" && pwd)"; shift 2 ;;
        --out) mkdir -p "$2"; out="$(cd "$2" && pwd)"; shift 2 ;;
        --asset) assets+=("$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"); shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ -n "$app" ] && [ -f "$app/AndroidManifest.xml" ] || { echo "--app must name a folder with AndroidManifest.xml" >&2; exit 2; }

sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
[ -d "$sdk" ] || { echo "set ANDROID_HOME to the Android SDK" >&2; exit 2; }
newest() { ls -d "$@" 2>/dev/null | sort -V | tail -n 1; }
ndk="${ANDROID_NDK_HOME:-$(newest "$sdk"/ndk/*)}"
[ -d "$ndk" ] || { echo "set ANDROID_NDK_HOME to an Android NDK" >&2; exit 2; }
platform_jar="${ANDROID_PLATFORM_JAR:-$(newest "$sdk"/platforms/android-*/android.jar)}"
tools="$(newest "$sdk"/build-tools/*)"
d8_jar="${D8_JAR:-$tools/lib/d8.jar}"
apksigner_jar="${APKSIGNER_JAR:-$tools/lib/apksigner.jar}"
runs() { "$@" >/dev/null 2>&1; }
# aapt and zipalign are host binaries; fall back to the PATH's (for example distribution packages) when the SDK's
# do not run on this host.
pick() {
    local name="$1" override="$2"
    if [ -n "$override" ]; then echo "$override"; return; fi
    if [ -x "$tools/$name" ] && runs "$tools/$name" version; then echo "$tools/$name"; return; fi
    if [ -x "$tools/$name" ] && [ "$name" = zipalign ] && "$tools/$name" 2>&1 | grep -q Zip; then echo "$tools/$name"; return; fi
    command -v "$name" || { echo "no usable $name" >&2; exit 2; }
}
aapt="$(pick aapt "${AAPT:-}")"
zipalign="$(pick zipalign "${ZIPALIGN:-}")"
for f in "$platform_jar" "$d8_jar" "$apksigner_jar"; do [ -f "$f" ] || { echo "missing $f" >&2; exit 2; }; done

# 1. native library
native="$out/native"
prebuilt="$(newest "$ndk"/toolchains/llvm/prebuilt/*)"
if [ -x "$prebuilt/bin/clang++" ] && runs "$prebuilt/bin/clang++" --version; then
    toolchain=(-DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26)
    strip="$prebuilt/bin/llvm-strip"
else
    toolchain=(-DCMAKE_TOOLCHAIN_FILE="$root/platform/android/host-clang.toolchain.cmake" -DANDROID_NDK="$ndk")
    strip="$(command -v llvm-strip || newest /usr/bin/llvm-strip-*)"
fi
generator=()
command -v ninja >/dev/null && generator=(-G Ninja)
cmake -S "$root" -B "$native" ${generator[@]+"${generator[@]}"} "${toolchain[@]}" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF >/dev/null
cmake --build "$native" --target atlanhost
library="$(find "$native" -name libatlanhost.so -type f | head -n 1)"

# 2. manifest, resources, assets
stage="$out/stage"
rm -rf "$stage"
mkdir -p "$stage/gen" "$stage/classes" "$stage/dex" "$stage/assets" "$stage/lib/arm64-v8a"
for a in ${assets[@]+"${assets[@]}"}; do cp "$a" "$stage/assets/"; done
asset_args=()
[ -n "$(ls -A "$stage/assets")" ] && asset_args=(-A "$stage/assets")
"$aapt" package -f -M "$app/AndroidManifest.xml" -S "$app/res" -I "$platform_jar" -J "$stage/gen" ${asset_args[@]+"${asset_args[@]}"} -F "$stage/base.apk"

# 3. Java: the host classes and the app's
find "$root/platform/android/java" "$app/java" "$stage/gen" -name '*.java' | sort > "$stage/sources.txt"
javac -nowarn -Xlint:-options -source 11 -target 11 -encoding UTF-8 -classpath "$platform_jar" -d "$stage/classes" @"$stage/sources.txt"
jar cf "$stage/classes.jar" -C "$stage/classes" .
java -cp "$d8_jar" com.android.tools.r8.D8 --release --min-api 26 --lib "$platform_jar" --output "$stage/dex" "$stage/classes.jar"

# 4. package, align, sign
"$strip" --strip-unneeded -o "$stage/lib/arm64-v8a/libatlanhost.so" "$library"
python3 -I - "$stage" <<'PY'
import sys, zipfile
stage = sys.argv[1]
with zipfile.ZipFile(stage + "/base.apk", "a", zipfile.ZIP_DEFLATED) as apk:
    apk.write(stage + "/dex/classes.dex", "classes.dex")
    apk.write(stage + "/lib/arm64-v8a/libatlanhost.so", "lib/arm64-v8a/libatlanhost.so")
PY
"$zipalign" -f 4 "$stage/base.apk" "$stage/aligned.apk"
keystore="$HOME/.android/debug.keystore"
if [ ! -f "$keystore" ]; then
    mkdir -p "$(dirname "$keystore")"
    keytool -genkeypair -keystore "$keystore" -storepass android -keypass android -alias androiddebugkey \
        -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -keysize 2048 -validity 10000 >/dev/null
fi
label="$(sed -n 's/.*package="\([^"]*\)".*/\1/p' "$app/AndroidManifest.xml" | head -n 1)"
apk="$out/$label-arm64-v8a.apk"
java -jar "$apksigner_jar" sign --ks "$keystore" --ks-pass pass:android --key-pass pass:android --min-sdk-version 26 --out "$apk" "$stage/aligned.apk"
java -jar "$apksigner_jar" verify "$apk"
echo "$apk"
