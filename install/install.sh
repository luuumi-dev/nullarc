#!/bin/bash
# nullarc installer. Run from the unpacked release folder with sudo, once the Mac boots with PhxBringup.kext in
# OpenCore (see INSTALL.md):
#
#   sudo ./install.sh                  Metal for apps on the Radeon 780M
#   sudo ./install.sh --windowserver   the same, and from the second boot on WindowServer composites on the GPU too
#   sudo ./install.sh --check          only the checks, nothing is changed
#
# It puts the boot-time GPU bring-up into /Users/Shared/gpu (run by the LaunchDaemon local.phxboot), PhxAccel.kext into
# the Auxiliary kernel collection and the Metal plugin with Mesa RADV into PhxGPU, linked from /Library/GPUBundles.
# Running it again upgrades in place. Takes effect at the next boot.
set -euo pipefail
SRC=$(cd "$(dirname "$0")" && pwd)
G=/Users/Shared/gpu
H=/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU
L=/Library/GPUBundles
TS=$(date +%Y%m%d-%H%M%S)

WS=0; CHECK=0
for a in "$@"; do
    case "$a" in
        --windowserver) WS=1 ;;
        --check) CHECK=1 ;;
        -h|--help) sed -n 2,11p "$0"; exit 0 ;;
        *) echo "unknown option $a (see --help)"; exit 2 ;;
    esac
done

say() { printf '%s\n' "$*"; }
fail() { say "  ✗ $*"; FAILED=1; }
FAILED=0

say "== checks"
[ "$(id -u)" = 0 ] || { say "  ✗ run it with sudo"; exit 1; }
ver=$(sw_vers -productVersion)
case "$ver" in 26.*) say "  ✓ macOS $ver" ;; *) fail "macOS $ver: nullarc is built for macOS Tahoe 26" ;; esac
args=$(nvram boot-args 2>/dev/null | cut -f2- || true)
for a in amfi=0x80 amfi_get_out_of_my_way=0x1 phxdebug=1; do
    case " $args " in *" $a "*) say "  ✓ boot-arg $a" ;; *) fail "boot-arg $a is missing (OpenCore NVRAM -> boot-args)" ;; esac
done
if grep -q "status: enabled" <<<"$(csrutil status)"; then fail "System Integrity Protection is on (OpenCore csr-active-config 7F080000)"
else say "  ✓ System Integrity Protection relaxed"; fi
# read the registry once: grep -q on a pipe would stop ioreg early, and pipefail counts that as a failure
IOR=$(ioreg -l -w0)
build=$(grep -m1 '"phx-build"' <<<"$IOR" | grep -oE '[0-9]+$' || true)
if [ -n "$build" ]; then say "  ✓ PhxBringup.kext is loaded (build $build)"
else fail "PhxBringup.kext is not loaded: add EFI/OC/Kexts/PhxBringup.kext to OpenCore and reboot first"; fi
if grep -qE '"compatible" = <[^>]*"pci1002,(15bf|15c8|1900|164e)"' <<<"$IOR"; then say "  ✓ Radeon 780M/760M/740M found"
else say "  ! no Radeon 780M/760M/740M (1002:15bf/15c8/1900/164e) found: other GPUs are not supported"; fi
for f in EFI/OC/Kexts/PhxBringup.kext Library/Extensions/PhxAccel.kext PhxGPU/NVMTLDriver.bundle PhxGPU/nvmtl/libvulkan_radeon.dylib \
         gpu/phxboot.sh gpu/full.sh gpu/bringup5.sh gpu/kinst.sh gpu/phxctl/phxctl gpu/phxctl/mesboot gpu/local.phxboot.plist; do
    [ -e "$SRC/$f" ] || fail "package file $f is missing"
done
[ "$(ls "$SRC"/gpu/fw/*.bin 2>/dev/null | wc -l)" -ge 9 ] || fail "package firmware gpu/fw is incomplete"
[ $FAILED = 0 ] || { say "Fix the items marked ✗ and run it again."; exit 1; }
[ $CHECK = 1 ] && { say "All checks passed; nothing was changed (--check)."; exit 0; }

say "== GPU bring-up -> $G"
mkdir -p "$G/phxctl" "$G/fw" "$G/backup"
for f in phxboot.sh full.sh bringup5.sh kinst.sh; do install -m 755 "$SRC/gpu/$f" "$G/$f"; done
install -m 755 "$SRC/gpu/phxctl/phxctl" "$SRC/gpu/phxctl/mesboot" "$G/phxctl/"
install -m 644 "$SRC"/gpu/fw/*.bin "$G/fw/"
chown -R root:wheel "$G"
rm -f "$G/phxboot-off" /var/db/phxboot.pending

say "== PhxAccel.kext -> Auxiliary kernel collection"
rm -rf /Library/Extensions/PhxAccel.kext
ditto "$SRC/Library/Extensions/PhxAccel.kext" /Library/Extensions/PhxAccel.kext
chown -R root:wheel /Library/Extensions/PhxAccel.kext; chmod -R go-w /Library/Extensions/PhxAccel.kext
BK="$G/backup" "$G/kinst.sh"

say "== Metal plugin -> $H"
mkdir -p "$H"
for n in NVMTLDriver.bundle nvmtl; do
    rm -rf "$H/.$n.new"; ditto "$SRC/PhxGPU/$n" "$H/.$n.new"
    # caches the plugin keeps next to the driver survive an upgrade
    if [ "$n" = nvmtl ]; then
        for c in aircache spvcache; do if [ -d "$H/nvmtl/$c" ]; then mv "$H/nvmtl/$c" "$H/.$n.new/"; fi; done
    fi
    # running processes keep the copy they mapped: move the old one aside instead of writing over it
    [ -e "$H/$n" ] && mv "$H/$n" "$H/.$n.old.$TS"
    mv "$H/.$n.new" "$H/$n"
    rm -rf "$H/.$n.old.$TS"
done
chown -R root:wheel "$H"
codesign --force --sign - "$H/NVMTLDriver.bundle" >/dev/null 2>&1
codesign --verify --strict "$H/NVMTLDriver.bundle" && say "  plugin signature ok"
mkdir -p "$L"
for n in NVMTLDriver.bundle nvmtl; do
    if [ -e "$L/$n" ] && [ ! -L "$L/$n" ]; then mkdir -p "/Library/GPUBundles-backup/$TS"; mv "$L/$n" "/Library/GPUBundles-backup/$TS/"; fi
    ln -sfn "$H/$n" "$L/$n"
done
# phxboot writes the real list at every boot; until then only the built-in test tools get the device
printf '# phxboot: Metal for the built-in test tools only\n' > "$L/nvmtl-allow.txt"; chmod 644 "$L/nvmtl-allow.txt"

say "== LaunchDaemon local.phxboot"
install -m 644 -o root -g wheel "$SRC/gpu/local.phxboot.plist" /Library/LaunchDaemons/local.phxboot.plist
# launchd remembers a disabled service even when its plist is replaced.
launchctl enable system/local.phxboot

if [ $WS = 1 ]; then
    touch "$G/phxboot-m2"
    say "== WindowServer on the GPU: on from the second boot (the first one checks the GPU comes up)"
else
    rm -f "$G/phxboot-m2"; nvram -d phxfb-boot 2>/dev/null || true
fi

say ""
say "Installed. Reboot now; /var/log/phxboot.log ends with \"done\" once the GPU is up."
say "Turn on automatic login (System Settings -> Users & Groups) if you used --windowserver."
