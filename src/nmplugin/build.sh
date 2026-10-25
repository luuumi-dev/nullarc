#!/bin/sh
# Builds the Metal plugin natively on the Mac (NVMTLDevice.m #includes every other source here).
# Usage: ./build.sh [Vulkan-Headers include dir]  ->  ./NVMTLDriver.new, installed by tools/phxctl/plugdeploy.sh
set -eu
cd "$(dirname "$0")"
VK=${1:-$HOME/gpu/Vulkan-Headers/include}
xcrun clang -arch x86_64 -bundle -fobjc-arc -O1 -Wno-protocol -Wno-incomplete-implementation -I"$VK" \
  -o NVMTLDriver.new NVMTLDevice.m -framework CoreFoundation -framework Foundation -framework IOKit -framework IOSurface -framework Metal
echo "built $(pwd)/NVMTLDriver.new"
