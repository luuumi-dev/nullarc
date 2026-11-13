#!/bin/bash
# After a system wake: if the GPU is in its power-on state (GRBM_STATUS reads 0 after a hibernation), bring it up
# again, then let WindowServer back in. Called by phxwaked (LaunchDaemon local.phxwake); log /var/log/phxwake.log.
#   kext side: PhxBringup holds submissions from the sleep until the queues are adopted again and drops stale VMID
#   bindings; PhxAMDFB debug.phxfb_wake=1 opens IOFramebuffer's paging gate; debug.nvaccelfb=9 closes the display
#   machine's pending mode change and enables the accelerator.
export HOME=/Users/Shared; G=/Users/Shared/gpu; P=$G/phxctl/phxctl
exec >>/var/log/phxwake.log 2>&1
echo "== $(date '+%F %T') wake"
[ -e $G/phxboot-off ] && { echo "GPU off (kill switch): nothing to do"; exit 0; }
ioreg -rc NMXAccel -d0 | grep -q NMXAccel || { echo "no accelerator: nothing to do"; exit 0; }
v=$($P regs 0x2004 | awk '{print $2}')
if [ "$v" != "0x00000000" ]; then echo "GPU still up (GRBM_STATUS $v): nothing to do"; exit 0; fi
t0=$(date +%s)
MC=1 CS=1 ADOPT=1 GFX=1 AG=1 bash $G/full.sh kiqsched | grep -E "^compute:|^gfx:|adopt"
killall shutdown 2>/dev/null          # full.sh arms a reboot in case the bring-up hangs the bus
sysctl -w debug.phxbos_flush=0 >/dev/null 2>&1
sysctl -w debug.phxfb_wake=1 >/dev/null
sysctl -w debug.nvaccelfb=9 >/dev/null
# what the boot does after the bring-up (gpuclk.sh): clock cap and clock gating
$P smu 0x1b 800 | tail -1
if [ -x /usr/local/libexec/gpucg.sh ] && [ ! -e $G/gpucg-off ]; then /usr/local/libexec/gpucg.sh all; fi
echo "done in $(( $(date +%s) - t0 )) s: GRBM_STATUS $($P regs 0x2004 | awk '{print $2}'), frames $(sysctl -n debug.nvaccel_iop_ok)"
