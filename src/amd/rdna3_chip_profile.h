//
//  rdna3_chip_profile.h — Per-chip facts for Phoenix (GC 11.0.1) and Phoenix 2 (GC 11.0.4), chosen from the
//  discovered IP versions, never from the PCI device id. Freestanding C++17, no allocations (kext and tests).
//
//  Every value cites the Linux drivers/gpu/drm/amd source it comes from. A chip whose GC / MP0 / MP1 versions are not
//  listed here gets no profile, and callers must refuse to touch the hardware.
//
//  chip_identity() classifies any RDNA 3 table (APU or discrete) without needing a register-layout profile; only
//  chip_profile() returns the register facts a caller needs before writing to the hardware.
//
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "rdna3_ip_discovery.h"

namespace rdna3 {

enum class PhxChip : uint8_t { Phoenix1, Phoenix2 };

// Unified memory (APU) or dedicated VRAM behind a PCIe BAR (discrete). Linux draws the same line:
// amdgpu_discovery.c sets AMD_IS_APU for GC 11.0.1 (Phoenix) and GC 11.0.4 (Phoenix 2) only, so every other
// GC 11.0.x part (Navi 31 = 11.0.0, Navi 33 = 11.0.2, Navi 32 = 11.0.3) is a discrete GPU. Mesa numbers the
// four parts gfx1100 / gfx1103 / gfx1102 / gfx1101 (src/amd/common/ac_gpu_info.c identify_chip).
enum class ChipKind : uint8_t { Apu = 0, Discrete = 1 };

struct IpVersion {
    uint8_t major, minor, revision;
};

enum class ChipFamily : uint8_t { Unknown = 0, Phoenix1, Phoenix2, Navi31, Navi32, Navi33 };

// What the discovery table says about the part, filled for both kinds of chip.
struct ChipIdentity {
    ChipFamily family;
    ChipKind   kind;
    IpVersion  gc, mp0, mp1;  // mp0/mp1 stay zero when the block is missing from the table
    bool       has_display;   // DMU (DCN) block present: the part can drive a display engine
};

const char *chip_family_name(ChipFamily f);
bool        chip_is_apu(ChipFamily f);

// Classifies a valid discovery table. Never fails on an unknown GC version: it returns family Unknown so that a
// caller can log the part and still refuse to program it.
ChipIdentity chip_identity(const IpDiscoveryParser &disc);

struct ChipProfile {
    PhxChip chip;
    const char *name;
    IpVersion gc, mp0, mp1;     // must all match the discovery table exactly,
    IpVersion mmhub, nbif;      // as must these (ChipRegs takes register offsets from their headers)
    // MP0 / MP1 segment (BASE_IDX) of the PSP and SMU mailbox registers.
    uint8_t psp_mbox_base_idx;
    uint8_t smu_mbox_base_idx;
    // amdgpu external_rev_id = rev_id + this (soc21.c, soc21_common_early_init); Mesa tells R1 from R2 by it.
    uint8_t external_rev_add;
    // Apu or Discrete. Discrete rows are added together with their register facts; until then chip_profile()
    // returns nullptr for them and no register access happens (docs/dgpu-support.md, stage 1).
    ChipKind kind = ChipKind::Apu;
    // False for a row whose register offsets are copied from the Linux headers but have not been confirmed against a
    // capture of the real card yet: chip_regs() refuses such a row, so it can only serve logging and planning.
    bool regs_verified = true;
};

// Absolute MMIO dword indices (BAR5) of the registers that depend on the chip's IP layout.
struct ChipRegs {
    uint32_t psp_c2pmsg_58, psp_c2pmsg_64, psp_c2pmsg_67, psp_c2pmsg_69, psp_c2pmsg_70, psp_c2pmsg_71, psp_c2pmsg_81;
    uint32_t smu_msg, smu_arg, smu_resp;
    uint32_t gb_addr_config;
    uint32_t mmhub_fb_location_base, mmhub_fb_location_top, mmhub_fb_offset;
    uint32_t nbio_remap_hdp_mem_flush_cntl;
};

// The profile for this discovery table, or nullptr (unknown chip: do not touch the hardware).
const ChipProfile *chip_profile(const IpDiscoveryParser &disc);

// Fills regs from the discovery table's segment bases. False if a needed IP or segment is missing.
bool chip_regs(const IpDiscoveryParser &disc, const ChipProfile &prof, ChipRegs &regs);

// Firmware file prefix for an IP, "<ip>_<major>_<minor>_<rev>" (amdgpu_ucode.c, amdgpu_ucode_ip_version_decode):
// gc for GC, psp for MP0, sdma for SDMA0. False if the IP is missing or out is too small.
bool fw_prefix(const IpDiscoveryParser &disc, HwId hw, char *out, size_t out_len);

} // namespace rdna3
