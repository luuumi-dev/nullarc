#!/bin/bash
# Phoenix GFX power-up in the Linux driver's order: PSP ring, TOC and TMR, the firmware of every engine through
# LOAD_IP_FW, RLC autoload, then the SMU messages that switch GFX on (EnableGfxImu with 1). Firmware from ~/gpu/fw at
# the offsets and sizes of linux-firmware's gc_11_0_1/psp_13_0_4 files. Run as root (phxboot) or as a user with sudo.
# A reboot is armed for 3 minutes in case the PSP hangs and cancelled at the end.
set -euo pipefail
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
P(){ $SUDO ~/gpu/phxctl/phxctl "$@"; }
# PSP system addresses bypass the IOMMU: reject missing or inconsistent layout before touching hardware.
FB_LAYOUT=$(ioreg -r -c PhxBringup -l -w0)
fb_number(){
    local value
    value=$(sed -n 's/.*"'"$1"'" = \([0-9][0-9]*\)$/\1/p' <<<"$FB_LAYOUT")
    [[ $value =~ ^[0-9]{1,15}$ ]] || { echo "invalid or missing $1" >&2; return 1; }
    printf '%s' "$((10#$value))"
}
FB_PHYS=$(fb_number phx-fb-phys)
FB_MC=$(fb_number phx-fb-mc-base)
FB_SIZE=$(fb_number phx-fb-size)
FB_APER=$(fb_number phx-fb-aper-len)
if (( FB_PHYS == 0 || FB_MC == 0 || (FB_PHYS & 0xffffff) || (FB_MC & 0xffffff) ||
      FB_SIZE < 0xc000000 || FB_APER < 0xc000000 )); then
    echo "framebuffer layout cannot contain the 64 MiB TMR at offset 128 MiB" >&2
    exit 1
fi
TOC_MC=$((FB_MC + 0x4000000))
TMR_MC=$((FB_MC + 0x8000000))
TMR_PHYS=$((FB_PHYS + 0x8000000))
echo "== watchdog: reboot in 3 min unless cancelled =="
WATCHDOG_PID=$($SUDO bash -c 'nohup shutdown -r +3 </dev/null >/dev/null 2>&1 & echo $!')
P psp destroy >/dev/null 2>&1 || true
P psp create 0x1100000 >/dev/null
P stage ~/gpu/fw/psp_13_0_4_toc.bin 256 2304 0x0 >/dev/null
P raw 0x20 "$((TOC_MC & 0xffffffff))" "$((TOC_MC >> 32))" 0x900 0 0 0 >/dev/null
P raw 0x5 "$((TMR_MC & 0xffffffff))" "$((TMR_MC >> 32))" 0x4000000 0x2 \
    "$((TMR_PHYS & 0xffffffff))" "$((TMR_PHYS >> 32))" >/dev/null
echo "== load firmware =="
L(){ P loadfw "$@" 2>&1 | grep loadfw >> ~/gpu/fwaddr.log; }
: > ~/gpu/fwaddr.log
L ~/gpu/fw/gc_11_0_1_pfp.bin 256 263168 2 0x000000
L ~/gpu/fw/gc_11_0_1_me.bin  256 263168 1 0x100000
L ~/gpu/fw/gc_11_0_1_mec.bin 256 267008 4 0x200000
L ~/gpu/fw/sdma_6_0_1.bin    256 17408 71 0x380000
L ~/gpu/fw/sdma_6_0_1.bin    17664 16896 72 0x3C0000
L ~/gpu/fw/gc_11_0_1_mes_2.bin 256 135648 33 0x400000
L ~/gpu/fw/gc_11_0_1_mes_2.bin 135904 131072 34 0x440000
L ~/gpu/fw/gc_11_0_1_mes1.bin 256 104272 81 0x480000
L ~/gpu/fw/gc_11_0_1_mes1.bin 104528 131072 82 0x4C0000
L ~/gpu/fw/gc_11_0_1_imu.bin 256 66048 68 0x500000
L ~/gpu/fw/gc_11_0_1_imu.bin 66304 66048 69 0x600000
L ~/gpu/fw/gc_11_0_1_rlc.bin 25344 2560 20 0x700000
L ~/gpu/fw/gc_11_0_1_rlc.bin 27904 21104 21 0x720000
L ~/gpu/fw/gc_11_0_1_rlc.bin 49008 66048 26 0x800000
L ~/gpu/fw/gc_11_0_1_rlc.bin 115056 33280 48 0x840000
L ~/gpu/fw/gc_11_0_1_rlc.bin 148336 8704 25 0x880000
L ~/gpu/fw/gc_11_0_1_rlc.bin 256 25088  8 0x300000
echo "== AUTOLOAD_RLC =="
P autoload
echo "== SMU 0x3, 0x2, EnableGfxImu(param=1) (linux order) =="
P smu 0x3 0 | tail -1
P smu 0x2 0 | tail -1
P smu 0x16 1 | tail -1
sleep 2
P regs 0xee7e 0x21a0 0x2004 0x263e
$SUDO /bin/kill "$WATCHDOG_PID" 2>/dev/null || true
echo "watchdog cancelled"
