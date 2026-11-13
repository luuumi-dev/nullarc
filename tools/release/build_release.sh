#!/bin/bash
# Build a nullarc release package on the reference Mac from what it runs, plus the scripts, documents and licenses of
# a repository checkout: tools/release/build_release.sh <version> <repo checkout> [out dir]
# Needs root (it reads the EFI partition). Leaves <out>/nullarc-<version>.zip, PhxBringup.kext-<version>.zip and
# SHA256SUMS. NVIDIA-only files of NullMoth's bundle and Apple binaries (air-opt) are left out.
set -euo pipefail
V=$1; R=$(cd "$2" && pwd); OUT=${3:-$PWD}
H=/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU
G=/Users/Shared/gpu
P=$OUT/nullarc-$V
FW="psp_13_0_4_toc.bin gc_11_0_1_pfp.bin gc_11_0_1_me.bin gc_11_0_1_mec.bin gc_11_0_1_mes_2.bin gc_11_0_1_mes1.bin
    gc_11_0_1_imu.bin gc_11_0_1_rlc.bin sdma_6_0_1.bin"

[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
rm -rf "$P"; mkdir -p "$P"/{EFI/OC/Kexts,Library/Extensions,PhxGPU/nvmtl,gpu/phxctl,gpu/fw,extras/firebat-u6c}

# the EFI partition of the disk macOS booted from (disk numbers move when other drives enumerate first)
STORE=$(diskutil apfs list "$(diskutil info / | awk -F': *' '/Part of Whole/ {print $2}')" | grep -m1 'Physical Store' | grep -oE 'disk[0-9]+s[0-9]+')
ESP=${STORE%s*}s1
EFI=$(diskutil info "$ESP" | awk -F': *' '/Mount Point/ {print $2}')
[ -n "$EFI" ] || { diskutil mount "$ESP" >/dev/null; EFI=$(diskutil info "$ESP" | awk -F': *' '/Mount Point/ {print $2}'); }
[ -d "$EFI/EFI/OC" ] || { echo "no OpenCore on $ESP"; exit 1; }
ditto "$EFI/EFI/OC/Kexts/PhxBringup.kext" "$P/EFI/OC/Kexts/PhxBringup.kext"
cp "$R/hardware/Sample.plist" "$R/hardware/REQUIRED_KEXTS.md" "$P/EFI/OC/"
ditto /Library/Extensions/PhxAccel.kext "$P/Library/Extensions/PhxAccel.kext"
# Installed bundles may predate the supported PCI IDs (GitHub #3 / #4).
python3 "$R/tools/release/check_pci_matches.py" "$R" "$P"

# the plugin as it runs, without what only NVIDIA GPUs use
ditto "$H/NVMTLDriver.bundle" "$P/PhxGPU/NVMTLDriver.bundle"
rm -f "$P"/PhxGPU/NVMTLDriver.bundle/Contents/MacOS/libnvdec_* "$P"/PhxGPU/NVMTLDriver.bundle/Contents/MacOS/libnvenc_*
rm -rf "$P/PhxGPU/NVMTLDriver.bundle/Contents/Resources/sass"
codesign --force --sign - "$P/PhxGPU/NVMTLDriver.bundle" >/dev/null 2>&1
# Mesa RADV, the Vulkan loader and its ICD manifest; not air-opt (an Apple binary) and not the shader caches
for f in libvulkan_radeon.dylib radeon_icd.json nvrm610.conf; do cp "$H/nvmtl/$f" "$P/PhxGPU/nvmtl/"; done
L=$(readlink "$H/nvmtl/libvulkan.1.dylib"); cp "$H/nvmtl/$L" "$P/PhxGPU/nvmtl/"
ln -s "$L" "$P/PhxGPU/nvmtl/libvulkan.1.dylib"; ln -s libvulkan.1.dylib "$P/PhxGPU/nvmtl/libvulkan.dylib"

# boot-time bring-up: scripts from the repository, tools built on the Mac, the firmware the scripts are written for
for f in phxboot.sh full.sh bringup5.sh kinst.sh local.phxboot.plist; do cp "$R/tools/phxctl/$f" "$P/gpu/"; done
cp "$G/phxctl/phxctl" "$G/phxctl/mesboot" "$P/gpu/phxctl/"
for f in $FW; do cp "$G/fw/$f" "$P/gpu/fw/"; done

cp "$R/install/install.sh" "$R/install/uninstall.sh" "$P/"
cp "$R/docs/INSTALL.md" "$R/docs/INSTALL_RU.md" "$R/LICENSE" "$R/THIRD_PARTY_NOTICES.md" "$P/"
cp -R "$R/LICENSES" "$P/"
cp "$R/hardware/acpi/SSDT-ADP1.aml" "$R/hardware/acpi/SSDT-ADP1.dsl" "$P/extras/firebat-u6c/"
cat > "$P/README.txt" <<EOF
nullarc $V — Metal on AMD Radeon 780M for macOS Tahoe

  INSTALL.md / INSTALL_RU.md   how to install, check, update and remove it
  install.sh, uninstall.sh     run with sudo from this folder
  EFI/OC/Kexts/PhxBringup.kext goes into your OpenCore EFI
  LICENSE, THIRD_PARTY_NOTICES.md, LICENSES/
                               the licenses of everything in this package; parts derived from NullMoth's
                               driver are for noncommercial use only (PolyForm Noncommercial 1.0.0)

https://github.com/luuumi-dev/nullarc
EOF
chown -R root:wheel "$P"; chmod 755 "$P"/install.sh "$P"/uninstall.sh "$P"/gpu/*.sh "$P"/gpu/phxctl/*

cd "$OUT"
rm -f "nullarc-$V.zip" "PhxBringup.kext-$V.zip"
ditto -c -k --sequesterRsrc --keepParent "nullarc-$V" "nullarc-$V.zip"
(cd "$P/EFI/OC/Kexts" && ditto -c -k --sequesterRsrc --keepParent PhxBringup.kext "$OUT/PhxBringup.kext-$V.zip")
shasum -a 256 "nullarc-$V.zip" "PhxBringup.kext-$V.zip" > SHA256SUMS
ls -la "nullarc-$V.zip" "PhxBringup.kext-$V.zip"; cat SHA256SUMS
