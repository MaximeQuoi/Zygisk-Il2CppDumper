#!/bin/bash
# Builds the Zygisk module without Gradle/CMake/Java, mirroring
# module/src/main/cpp/CMakeLists.txt (release flags) and module/build.gradle
# (Magisk packaging).
#
#   scripts/build-ndk.sh <path to android-ndk-r25c> [build dir]
set -euo pipefail

NDK="$1"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${2:-$ROOT/build-ndk}"
TC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin"
SRC="$ROOT/module/src/main/cpp"
rm -rf "$WORK" && mkdir -p "$WORK"

eval "$(sed -nE 's/^ *(moduleVersionCode|moduleVersion|magiskModuleId|moduleName|moduleAuthor|moduleDescription) = (.*)$/\1=\2/p' "$ROOT/module.gradle")"

CFLAGS="-Werror=format -fdata-sections -ffunction-sections -O2 -fvisibility=hidden -fvisibility-inlines-hidden -fPIC -D_GNU_SOURCE -DNDEBUG -I$SRC/xdl/include"
CXXFLAGS="-std=c++20 -fno-exceptions -fno-rtti"
LDFLAGS="-shared -Wl,--hash-style=both -Wl,-exclude-libs,ALL -Wl,--gc-sections -Wl,--strip-all -static-libstdc++ -llog -Wl,--build-id=sha1 -Wl,--no-undefined -Wl,-z,max-page-size=16384"

MOD="$WORK/magisk_module"
mkdir -p "$MOD/zygisk"

for pair in arm64-v8a:aarch64-linux-android23 armeabi-v7a:armv7a-linux-androideabi23 x86:i686-linux-android23 x86_64:x86_64-linux-android23; do
  abi="${pair%%:*}"; triple="${pair#*:}"
  obj="$WORK/obj/$abi"; mkdir -p "$obj"
  extra=""; [ "$abi" = arm64-v8a ] && extra="-ffixed-x18"
  objs=()
  for f in "$SRC"/xdl/*.c; do
    o="$obj/$(basename "$f").o"
    "$TC/clang" --target="$triple" $CFLAGS $extra -c "$f" -o "$o"; objs+=("$o")
  done
  for f in "$SRC"/*.cpp; do
    o="$obj/$(basename "$f").o"
    "$TC/clang++" --target="$triple" $CFLAGS $CXXFLAGS $extra -c "$f" -o "$o"; objs+=("$o")
  done
  "$TC/clang++" --target="$triple" $extra "${objs[@]}" $LDFLAGS -o "$MOD/zygisk/$abi.so"
  "$TC/llvm-strip" --strip-all --remove-section=.comment "$MOD/zygisk/$abi.so"
  echo "built $abi ($(stat -f%z "$MOD/zygisk/$abi.so") bytes)"
done

cp -R "$ROOT/template/magisk_module/META-INF" "$MOD/"
sed -e "s|\${id}|$magiskModuleId|" -e "s|\${name}|$moduleName|" -e "s|\${version}|$moduleVersion|" \
    -e "s|\${versionCode}|$moduleVersionCode|" -e "s|\${author}|$moduleAuthor|" \
    -e "s|\${description}|$moduleDescription|" "$ROOT/template/magisk_module/module.prop" | tr -d '\r' > "$MOD/module.prop"

mkdir -p "$ROOT/out"
ZIP="$ROOT/out/${magiskModuleId//_/-}-${moduleVersion}-release.zip"
rm -f "$ZIP"
(cd "$MOD" && zip -qr -X "$ZIP" META-INF module.prop zygisk)
echo "zip: $ZIP"
