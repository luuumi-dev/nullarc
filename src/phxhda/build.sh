#!/bin/sh
# Builds PhxHDA.kext (a Lilu plugin) natively on macOS with the Command Line Tools.
# Usage: ./build.sh <path-to-MacKernelSDK> <path-to-Lilu source (the folder that holds Headers/ and Library/)>
set -eu
MKSDK=${1:?usage: build.sh <MacKernelSDK> <Lilu>}
LILU=${2:?usage: build.sh <MacKernelSDK> <Lilu>}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/build/PhxHDA.kext"
rm -rf "$HERE/build"
mkdir -p "$OUT/Contents/MacOS"

CFLAGS="-arch x86_64 -mmacosx-version-min=11.0 -mkernel -fno-builtin -fno-common -nostdinc
  -I$MKSDK/Headers -I$LILU -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -Os -Wall
  -DPRODUCT_NAME=PhxHDA -DMODULE_VERSION=1.0.0"

clang -x c $CFLAGS -c "$HERE/kmod_info.c" -o "$HERE/build/kmod_info.o"
for f in "$HERE/PhxHDA.cpp" "$LILU/Library/plugin_start.cpp"; do
  clang++ $CFLAGS -std=c++17 -fapple-kext -fno-rtti -fno-exceptions -c "$f" -o "$HERE/build/$(basename "$f" .cpp).o"
done
clang++ -arch x86_64 -mmacosx-version-min=11.0 -nostdlib -Xlinker -kext \
  -L"$MKSDK/Library/x86_64" -lkmod -lcc_kext \
  "$HERE/build/kmod_info.o" "$HERE/build/PhxHDA.o" "$HERE/build/plugin_start.o" \
  -o "$OUT/Contents/MacOS/PhxHDA"
cp "$HERE/Info.plist" "$OUT/Contents/Info.plist"
echo "built $OUT"
