#!/bin/bash
# Rebuild the Auxiliary kernel collection from /Library/Extensions (PhxAccel, and AppleHDA when present: Tahoe dropped
# it from the system, and kmutil only takes com.apple bundles given by path) and publish it after a check; the old one
# is kept in $BK (default /Users/Shared/gpu/backup). Run as root; takes effect at the next boot.
set -euo pipefail
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc; NEW=$KC.phx-new; BK=${BK:-/Users/Shared/gpu/backup}; TS=$(date +%Y%m%d-%H%M%S)
EXTRA=(); WANT=(); [ -d /Library/Extensions/PhxAccel.kext ] && { EXTRA+=(--bundle-path /Library/Extensions/PhxAccel.kext); WANT+=(com.luuumi.PhxAccel); }
H=/Library/Extensions/AppleHDA.kext
if [ -d $H ]; then
  for b in $H $H/Contents/PlugIns/IOHDAFamily.kext $H/Contents/PlugIns/AppleHDAController.kext $H/Contents/PlugIns/DspFuncLib.kext \
           $H/Contents/PlugIns/AppleHDAHardwareConfigDriver.kext $H/Contents/PlugIns/AppleMikeyDriver.kext; do EXTRA+=(--bundle-path $b); done
  WANT+=(com.apple.driver.AppleHDA com.apple.iokit.IOHDAFamily com.apple.driver.AppleHDAController)
fi
rm -f $NEW
kmutil create -n aux --volume-root / --allow-missing-kdk --kernel /System/Library/Kernels/kernel \
  -B /System/Library/KernelCollections/BootKernelExtensions.kc -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
  --repository /Library/Extensions --repository /Library/Apple/System/Library/Extensions \
  ${EXTRA[@]+"${EXTRA[@]}"} -A $NEW -z > /tmp/phx-kmutil.log 2>&1
INS=$(kmutil inspect -a x86_64 -A $NEW 2>/dev/null)
for b in ${WANT[@]+"${WANT[@]}"}; do grep -q "^$b[[:space:]]" <<<"$INS" || { echo "  MISSING: $b -- not publishing"; tail -5 /tmp/phx-kmutil.log; exit 1; }; done
echo "$INS" | awk '{print "  " $1, $2}'
mkdir -p $BK; [ -f $KC ] && cp -p $KC $BK/aux.kc.$TS
mv -f $NEW $KC && echo "aux KC published (old one, if any: $BK/aux.kc.$TS)"
