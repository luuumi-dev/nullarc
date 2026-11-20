#!/bin/sh
# Builds PhxCPPC.kext natively on macOS with the Command Line Tools.
# Usage: ./build.sh <path-to-MacKernelSDK>
set -eu
MKSDK=${1:?usage: build.sh <MacKernelSDK>}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/build/PhxCPPC.kext"
rm -rf "$HERE/build"
mkdir -p "$OUT/Contents/MacOS"

CFLAGS="-arch x86_64 -mmacosx-version-min=11.0 -mkernel -fno-builtin -fno-common -nostdinc
  -I$MKSDK/Headers -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -Os -Wall"

clang -x c $CFLAGS -c "$HERE/kmod_info.c" -o "$HERE/build/kmod_info.o"
clang++ $CFLAGS -std=c++17 -fapple-kext -fno-rtti -fno-exceptions \
  -c "$HERE/PhxCPPC.cpp" -o "$HERE/build/PhxCPPC.o"
clang++ -arch x86_64 -mmacosx-version-min=11.0 -nostdlib -Xlinker -kext \
  -L"$MKSDK/Library/x86_64" -lkmod -lcc_kext \
  "$HERE/build/kmod_info.o" "$HERE/build/PhxCPPC.o" \
  -o "$OUT/Contents/MacOS/PhxCPPC"
cp "$HERE/Info.plist" "$OUT/Contents/Info.plist"
echo "built $OUT"
