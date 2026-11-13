#!/bin/bash
# nullarc uninstaller: sudo ./uninstall.sh. Removes what install.sh put on the system volume and the Data volume;
# PhxBringup.kext and the boot-args stay in OpenCore until you take them out (see INSTALL.md). Reboot afterwards.
set -uo pipefail
[ "$(id -u)" = 0 ] || { echo "run it with sudo"; exit 1; }
G=/Users/Shared/gpu
H=/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU
L=/Library/GPUBundles

echo "== boot-time bring-up"
rm -f /Library/LaunchDaemons/local.phxboot.plist /var/db/phxboot.pending
nvram -d phxfb-boot 2>/dev/null

echo "== Metal plugin"
for n in NVMTLDriver.bundle nvmtl; do [ -L "$L/$n" ] && rm -f "$L/$n"; done
rm -f "$L/nvmtl-allow.txt"
rm -rf "$H"

echo "== PhxAccel.kext"
rm -rf /Library/Extensions/PhxAccel.kext
# Rebuild from remaining extensions; a failed rebuild must not delete their working collection.
if ! { [ -x "$G/kinst.sh" ] && BK=/var/tmp "$G/kinst.sh"; }; then
    echo "  rebuild failed: kept the existing Auxiliary kernel collection and $G for recovery" >&2
    exit 1
fi

rm -rf "$G"
echo ""
echo "Removed. Now take PhxBringup.kext out of OpenCore (EFI/OC/Kexts and Kernel -> Add) and, if nothing else"
echo "needs them, the amfi=0x80 amfi_get_out_of_my_way=0x1 phxdebug=1 boot-args, then reboot."
