#!/bin/bash
# usbcap.sh [seconds]: (root, on the Mac, in the background) records what the Mac does while a USB-C device is plugged in.
# Output in /Users/Shared/gpu/usbc/: pm.txt (interrupts and per-task CPU each second), kprof.txt (hottest kernel
# stacks), klog.txt (kernel log), usb.txt (USB tree at the end). /Users/Shared/gpu/usbc/state reads END when done.
T=${1:-60}; O=/Users/Shared/gpu/usbc; rm -rf $O; mkdir -p $O; echo running > $O/state
powermetrics --samplers interrupts,tasks --show-process-energy -i 1000 -n $T > $O/pm.txt 2>&1 &
dtrace -q -x stackframes=14 -n "profile-199 /arg0/ { @[stack()] = count(); } tick-${T}s { trunc(@, 25); printa(@); exit(0); }" > $O/kprof.txt 2>&1 &
/usr/bin/log stream --style compact --predicate 'process == "kernel" AND NOT eventMessage CONTAINS "skywalk" AND NOT eventMessage CONTAINS "IO80211"' > $O/klog.txt 2>&1 &
L=$!
for i in $(seq 1 $T); do date +%s >> $O/ticks; sleep 1; done
kill $L 2>/dev/null; wait
ioreg -p IOUSB -w0 -l | grep -E '^[ |]*\+-o|"USB Product Name"|"Device Speed"|"idVendor"|"idProduct"' > $O/usb.txt
echo END > $O/state
