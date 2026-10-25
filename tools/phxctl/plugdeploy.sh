#!/bin/bash
# plugdeploy.sh "<conf lines>" [plugin]: (root, on the Mac) installs a built NVMTLDriver (default ~/gpu/nmplugin/NVMTLDriver.new,
# from src/nmplugin/build.sh) into PhxGPU, the old one backed up to /Users/Shared/gpu/backup, and writes nvrm610.conf
# (KEY=VALUE lines, \n-separated; today "NVMTL_WS_WAIT_INTERNAL=1\n"). New processes load it at once, WindowServer after a reboot.
set -e
H=/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU/NVMTLDriver.bundle; B=$H/Contents/MacOS
C=/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU/nvmtl/nvrm610.conf
SRC=${2:-/Users/dripside/gpu/nmplugin/NVMTLDriver.new}
TS=$(date +%Y%m%d-%H%M%S)
mkdir -p /Users/Shared/gpu/backup && cp $B/NVMTLDriver /Users/Shared/gpu/backup/NVMTLDriver.$TS
cp "$SRC" $B/.n && chown root:wheel $B/.n && chmod 755 $B/.n && mv $B/.n $B/NVMTLDriver
codesign --force --sign - $H >/dev/null 2>&1; codesign --verify --strict $H && echo "plugin installed, signature ok (old: NVMTLDriver.$TS)"
printf '%b' "${1:-}" > $C; chmod 644 $C; echo "conf: $(tr '\n' ' ' < $C)"
