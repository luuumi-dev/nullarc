#!/bin/bash
# pmwatch.sh [seconds=1] [count=0 (forever)]: one line per interval from PhxBringup's SMU telemetry (debug.phx_pm_*):
# GFX clock now/filtered, GFX busy %, the SoftMax the kext set, package / cores / APU power, GFX temperature.
# Needs PhxBringup build 35+. The sysctls appear with the first user client (phxboot, WindowServer or phxctl).
set -u
IV=${1:-1}; N=${2:-0}
S(){ sysctl -n "debug.phx_pm_$1" 2>/dev/null || echo "?"; }
if ! sysctl -n debug.phx_pm_state >/dev/null 2>&1; then
    echo "debug.phx_pm_* missing: PhxBringup < 35, or no client has opened it yet (run: phxctl smuinfo)"; exit 1
fi
echo "state $(S state)  mode $(S mode)  DPM $(S dpm_min)..$(S dpm_max) MHz  table page was clear: $(S vram_was_zero)"
printf "%8s %6s %6s %5s %5s %7s %8s %8s %7s %7s %6s %5s\n" time gfxMHz avgMHz busy% avg% softmax sock_mW sockAvg core_mW apu_mW gfx_C errs
i=0
while :; do
    t=$(S gfx_temp); [ "$t" != "?" ] && t=$((t / 100))
    printf "%8s %6s %6s %5s %5s %7s %8s %8s %7s %7s %6s %5s\n" "$(date +%H:%M:%S)" "$(S gfxclk)" "$(S gfxclk_avg)" \
        "$(S busy)" "$(S busy_avg)" "$(S softmax)" "$(S socket_mw)" "$(S socket_mw_avg)" "$(S core_mw_avg)" \
        "$(S apu_mw)" "$t" "$(S errors)"
    i=$((i + 1)); [ "$N" -gt 0 ] && [ "$i" -ge "$N" ] && break
    sleep "$IV"
done
