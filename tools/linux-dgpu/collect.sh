#!/bin/bash
# tools/linux-dgpu/collect.sh — read-only hardware capture of an RDNA 3 discrete GPU for nullarc.
#
# Produces a directory of files a maintainer can compare against the Phoenix APU baseline:
# PCI identity and BARs, the VBIOS ROM, the IP discovery binary (the register map of every block),
# the amdgpu bring-up log, module parameters, firmware inventory and the DRM topology JSON.
#
# Nothing is written to the GPU or to the driver: the only writes are to the output directory, plus the
# standard one-byte enable/disable of /sys/.../rom around the VBIOS read. No register access.
#
# Usage:  sudo ./collect.sh [--skip-vbios] [PCI-BDF]
#         (BDF is only needed when the system has more than one AMD display device)
#
# See tools/linux-dgpu/README.md for what is safe to attach to a pull request.

set -u

OUT=""
SKIP_VBIOS=0
BDF=""

for arg in "$@"; do
    case "$arg" in
        --skip-vbios) SKIP_VBIOS=1 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        0000:*|*:*:*.*) BDF="$arg" ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [ "$(id -u)" != "0" ]; then
    echo "collect.sh needs root: the VBIOS ROM and the IP discovery table are root-only." >&2
    echo "Run: sudo $0 $*" >&2
    exit 1
fi

say() { printf '%s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# ---- find the GPU --------------------------------------------------------------------------------------

# APU ids nullarc already supports; if both an APU and a dGPU are present we want the dGPU.
apu_id() {
    case "$1" in
        1900|15bf|15c8|164e|15d8|1638|164c) return 0 ;;
        *) return 1 ;;
    esac
}

if [ -z "$BDF" ]; then
    declare -a cands=()
    if have lspci; then
        while read -r bdf class id; do
            case "$class" in
                0300:*|0302:*|0380:*) ;;
                *) continue ;;
            esac
            case "$id" in
                1002:*) ;;
                *) continue ;;
            esac
            cands+=("$bdf|$id")
        done < <(lspci -Dn | awk '{print $1, $2, $3}')
    fi
    if [ "${#cands[@]}" -eq 0 ]; then
        echo "no AMD display device found (is amdgpu loaded and is lspci installed?)" >&2
        exit 1
    fi
    for c in "${cands[@]}"; do
        b="${c%%|*}"; d="${c##*:}"
        if ! apu_id "$d"; then BDF="$b"; break; fi
    done
    if [ -z "$BDF" ]; then BDF="${cands[0]%%|*}"; fi
    if [ "${#cands[@]}" -gt 1 ]; then
        say "several AMD display devices found; using $BDF"
        for c in "${cands[@]}"; do say "  ${c%%|*}  ${c##*|}"; done
        say "re-run with an explicit BDF to capture another one"
    fi
fi

DEV="/sys/bus/pci/devices/$BDF"
if [ ! -d "$DEV" ]; then echo "no such PCI device: $BDF" >&2; exit 1; fi

VENDOR=$(cat "$DEV/vendor" 2>/dev/null)
DEVICE=$(cat "$DEV/device" 2>/dev/null)
if [ "$VENDOR" != "0x1002" ]; then echo "$BDF is not an AMD device ($VENDOR:$DEVICE)" >&2; exit 1; fi

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="${OUT_DIR:-dgpu-capture-$BDF-$STAMP}"
mkdir -p "$OUT" || exit 1
say "capturing $BDF ($VENDOR:$DEVICE) into $OUT"

# ---- identity, BARs, link -------------------------------------------------------------------------------

{
    printf 'bdf %s\nvendor %s\ndevice %s\nsubsystem_vendor %s\nsubsystem_device %s\nrevision %s\nclass %s\n' \
        "$BDF" "$VENDOR" "$DEVICE" \
        "$(cat "$DEV/subsystem_vendor" 2>/dev/null)" \
        "$(cat "$DEV/subsystem_device" 2>/dev/null)" \
        "$(cat "$DEV/revision" 2>/dev/null)" \
        "$(cat "$DEV/class" 2>/dev/null)"
    printf 'driver %s\n' "$(basename "$(readlink -f "$DEV/driver" 2>/dev/null)" 2>/dev/null)"
    printf 'kernel %s\n' "$(uname -r)"
    printf 'distro %s\n' "$( (. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME") || echo unknown)"
    printf 'date_utc %s\n' "$STAMP"
} > "$OUT/identity.txt"

if have lspci; then
    lspci -D -vvv -s "$BDF" > "$OUT/lspci-vvv.txt" 2>&1
    lspci -D -nn -s "$BDF" > "$OUT/lspci-nn.txt" 2>&1
    # Every BAR, with its size: this is where a 256 MiB window vs a 8 GiB ReBAR window shows up.
    awk '/^[[:space:]]*(Region|Capabilities: \[[0-9a-f]+\] Power|LnkSta|LnkCap|BAR [0-9])/ {print}' \
        "$OUT/lspci-vvv.txt" > "$OUT/bars.txt" 2>/dev/null
else
    echo "lspci not installed; BAR and link data missing" > "$OUT/lspci-vvv.txt"
fi

dd if="$DEV/config" of="$OUT/pci-config.bin" bs=64 count=1 2>/dev/null
if have hexdump; then hexdump -C "$OUT/pci-config.bin" > "$OUT/pci-config.txt"; fi

# ---- mVBIOS ---------------------------------------------------------------------------------------------

if [ "$SKIP_VBIOS" -eq 0 ]; then
    # Standard read window: enable, read 64 KiB, disable. The ROM is a few hundred KiB at most.
    if echo 1 > "$DEV/rom" 2>/dev/null; then
        dd if="$DEV/rom" of="$OUT/vbios.rom" bs=64k 2>/dev/null
        echo 0 > "$DEV/rom" 2>/dev/null
        if have sha256sum; then sha256sum "$OUT/vbios.rom" > "$OUT/vbios.rom.sha256"; fi
        say "VBIOS: $(stat -c %s "$OUT/vbios.rom" 2>/dev/null || echo 0) bytes"
    else
        echo "could not enable $DEV/rom (CONFIG_ROM or firmware restriction); capture the VBIOS on Windows with GPU-Z instead" \
            > "$OUT/vbios-error.txt"
    fi
fi

# ACPI VFCT carries a copy of the VBIOS on UEFI systems; it is readable even when /rom is not.
if [ -r /sys/firmware/acpi/tables/VFCT ]; then
    cp /sys/firmware/acpi/tables/VFCT "$OUT/VFCT.bin" 2>/dev/null
fi

# ---- IP discovery: the register map of every block -------------------------------------------------------

found_discovery=0
for d in /sys/kernel/debug/dri/*/amdgpu; do
    [ -r "$d/ip_discovery" ] || continue
    idx=$(basename "$(dirname "$d")")
    # Only keep the debugfs entry that belongs to this PCI device, when the driver exposes the link.
    if [ -e "$d/device" ] || [ -L "$d/device" ]; then
        link=$(readlink -f "$d/device" 2>/dev/null || true)
        case "$link" in
            *"$BDF") ;;
            *) [ -n "$link" ] && continue ;;
        esac
    fi
    cp "$d/ip_discovery" "$OUT/ip-discovery-$idx.bin" 2>/dev/null && found_discovery=1
    if have hexdump; then hexdump -C "$OUT/ip-discovery-$idx.bin" > "$OUT/ip-discovery-$idx.hex" 2>/dev/null; fi
done
if [ "$found_discovery" -eq 0 ]; then
    # Fallback: take the first table and label it, so the data is never silently missing.
    for d in /sys/kernel/debug/dri/*/amdgpu; do
        [ -r "$d/ip_discovery" ] || continue
        cp "$d/ip_discovery" "$OUT/ip-discovery.bin" 2>/dev/null && found_discovery=1
        break
    done
fi
if [ "$found_discovery" -eq 0 ]; then
    echo "ip_discovery not readable (needs debugfs mounted and amdgpu loaded):" > "$OUT/ip-discovery-error.txt"
    echo "  mount -t debugfs none /sys/kernel/debug" >> "$OUT/ip-discovery-error.txt"
fi

# Decode it with the repository's own parser when the script sits inside a checkout.
REPO_ROOT=$(cd "$(dirname "$0")/../.." 2>/dev/null && pwd)
for f in "$OUT"/ip-discovery*.bin; do
    [ -f "$f" ] || continue
    if [ -n "$REPO_ROOT" ] && [ -f "$REPO_ROOT/tools/parse_discovery.py" ] && have python3; then
        python3 "$REPO_ROOT/tools/parse_discovery.py" "$f" > "${f%.bin}.txt" 2>&1
    fi
done

# ---- driver log and parameters ---------------------------------------------------------------------------

dmesg --color=never 2>/dev/null | grep -iE "amdgpu|psp|smu|rlc|mes|gmc|dcn|dmub|link training" \
    > "$OUT/dmesg-amdgpu.txt"
dmesg --color=never 2>/dev/null > "$OUT/dmesg-full.txt"

{
    echo "== module parameters =="
    if [ -r /sys/module/amdgpu/parameters ]; then
        for p in /sys/module/amdgpu/parameters/*; do
            printf '%-28s %s\n' "$(basename "$p")" "$(cat "$p" 2>/dev/null)"
        done
    fi
    echo
    echo "== amdgpu module version =="
    modinfo amdgpu 2>/dev/null | grep -E "^(filename|version|srcversion|vermagic|firmware)" | head -40
} > "$OUT/amdgpu-module.txt"

# ---- memory, topology ------------------------------------------------------------------------------------

{
    for f in mem_info_vram_total mem_info_vram_used mem_info_gtt_total mem_info_gtt_used \
             mem_info_vis_vram_total mem_info_vis_vram_used; do
        [ -r "$DEV/$f" ] && printf '%-24s %s\n' "$f" "$(cat "$DEV/$f")"
    done
    for link in "$DEV"/drm/card* "$DEV"/drm/renderD*; do
        [ -e "$link" ] || continue
        printf '%-24s %s\n' "$(basename "$link")" "$(readlink -f "$link")"
    done
} > "$OUT/memory.txt"

# DRM topology JSON through the repository's existing read-only collector.
RENDER=$(ls -d "$DEV"/drm/renderD* 2>/dev/null | head -1)
if [ -n "$RENDER" ] && [ -n "$REPO_ROOT" ] && [ -f "$REPO_ROOT/tools/linux-profile/collect.c" ] && have cc; then
    TMPBIN=$(mktemp /tmp/dgpu-collect.XXXXXX)
    if cc -std=c11 -Wall -O2 $(pkg-config --cflags libdrm 2>/dev/null) \
            "$REPO_ROOT/tools/linux-profile/collect.c" -o "$TMPBIN" 2>"$OUT/topology-build.log"; then
        "$TMPBIN" "/dev/dri/$(basename "$RENDER")" > "$OUT/topology.json" 2> "$OUT/ip-blocks.txt"
        say "topology: $OUT/topology.json"
    fi
    rm -f "$TMPBIN"
fi

# ---- firmware inventory ----------------------------------------------------------------------------------

{
    echo "== firmware referenced by amdgpu =="
    modinfo -F firmware amdgpu 2>/dev/null | tr ' ' '\n' | sed '/^$/d' | sort -u
} > "$OUT/firmware-all.txt"

FWPATTERN='^(gc_11_0_|psp_13_0_|smu_13_0_|sdma_6_0_|dcn_3_2_|vcn_4_0_|mmhub|nbio_7_7)'
if [ -d /lib/firmware/amdgpu ]; then
    ls /lib/firmware/amdgpu | grep -E "$FWPATTERN" | sed 's/\.zst$//' | sort -u > "$OUT/firmware-rdna3.txt"
    if have sha256sum; then
        while read -r f; do
            for cand in "/lib/firmware/amdgpu/$f" "/lib/firmware/amdgpu/$f.zst" \
                        "/lib/firmware/updates/amdgpu/$f" "/lib/firmware/updates/amdgpu/$f.zst"; do
                [ -f "$cand" ] && { sha256sum "$cand"; break; }
            done
        done < "$OUT/firmware-rdna3.txt" > "$OUT/firmware-rdna3.sha256"
    fi
fi

# ---- summary ---------------------------------------------------------------------------------------------

if have sha256sum; then
    ( cd "$OUT" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS )
fi

say ""
say "captured into $OUT"
ls -1 "$OUT" | sed 's/^/  /'
say ""
if [ -f "$OUT/ip-discovery.bin" ] || ls "$OUT"/ip-discovery-*.bin >/dev/null 2>&1; then
    say "Attach to the pull request: identity.txt lspci-vvv.txt bars.txt dmesg-amdgpu.txt"
    say "  amdgpu-module.txt memory.txt ip-discovery*.bin ip-discovery*.txt topology.json"
    say "Do NOT attach: vbios.rom, VFCT.bin (AMD/OEM copyright, no redistribution grant). Share the"
    say "  VBIOS privately with a maintainer if they ask; the .gitignore already excludes *.rom."
else
    say "IP discovery was not captured — see $OUT/ip-discovery-error.txt"
fi
