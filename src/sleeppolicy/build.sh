#!/bin/sh
set -eu
MKSDK=${1:?usage: build.sh <MacKernelSDK>}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/build/WujieSleepPolicy.kext"
rm -rf "$HERE/build"; mkdir -p "$OUT/Contents/MacOS"
clang++ -arch x86_64 -mmacosx-version-min=11.0 -fapple-kext -mkernel -nostdinc -I"$MKSDK/Headers" \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -O2 \
  -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -c "$HERE/WujieSleepPolicy.cpp" -o "$HERE/build/x.o"
clang -arch x86_64 -mmacosx-version-min=11.0 -mkernel -nostdinc -I"$MKSDK/Headers" -DKERNEL -DKERNEL_PRIVATE -O2 \
  -c "$HERE/info.c" -o "$HERE/build/info.o"
clang++ -arch x86_64 -mmacosx-version-min=11.0 -nostdlib -Xlinker -kext -L"$MKSDK/Library/x86_64" \
  -lkmodc++ -lkmod -lcc_kext "$HERE/build/x.o" "$HERE/build/info.o" -o "$OUT/Contents/MacOS/WujieSleepPolicy"
cp "$HERE/Info.plist" "$OUT/Contents/Info.plist"
codesign --force --sign - "$OUT"
echo "built $OUT"
