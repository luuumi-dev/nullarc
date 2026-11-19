#!/bin/bash
# On the Mac (root): move the Metal plugin and its Vulkan stack out of /Library/GPUBundles into
# /System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU and leave symlinks behind. Sandboxed GPU
# processes (WebKit's com.apple.WebKit.GPU, Chromium/Electron helpers) may map executables only from system paths
# (com.apple.WebKit.GPUProcess.sb allows file-map-executable in /System/Library/CoreServices, but /Library/GPUBundles
# only for reading); that CoreTypes directory is a firmlink into the writable Data volume, so it passes the check.
# The originals stay in /Library/GPUBundles-backup (running processes keep their mapped copies). Idempotent.
set -eu
H=/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU; G=/Library/GPUBundles; B=/Library/GPUBundles-backup
# A root umask of 077 can create a 0700 symlink. On macOS that blocks
# ordinary-user bundle class lookup even when its executable is readable.
# Change only the Metal alias itself; never follow it or chmod its target.
metal_link_mode() {
    [ "$(readlink "$G/NVMTLDriver.bundle")" = "$H/NVMTLDriver.bundle" ] || {
        echo "NVMTLDriver.bundle: unexpected link target" >&2; return 1;
    }
    chmod -h 755 "$G/NVMTLDriver.bundle"
}
mkdir -p "$H" "$B"
for n in NVMTLDriver.bundle nvmtl; do
    if [ -L "$G/$n" ]; then
        if [ "$n" = NVMTLDriver.bundle ]; then metal_link_mode; fi
        echo "$n: already a link to $(readlink "$G/$n")"; continue
    fi
    rm -rf "$H/$n"; cp -Rp "$G/$n" "$H/"
    rm -rf "$B/$n"; mv "$G/$n" "$B/$n"
    ln -s "$H/$n" "$G/$n"
    if [ "$n" = NVMTLDriver.bundle ]; then metal_link_mode; fi
    echo "$n -> $H/$n"
done
codesign --verify --strict "$H/NVMTLDriver.bundle" && echo "plugin signature ok"
