//
//  rdna3_chip_profile.h — Per-chip facts for Phoenix (GC 11.0.1) and Phoenix 2 (GC 11.0.4), chosen from the
//  discovered IP versions, never from the PCI device id. Freestanding C++17, no allocations (kext and tests).
//
//  Every value cites the Linux drivers/gpu/drm/amd source it comes from. A chip whose GC / MP0 / MP1 versions are not
//  listed here gets no profile, and callers must refuse to touch the hardware.
//
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "rdna3_ip_discovery.h"

namespace rdna3 {

enum class PhxChip : uint8_t { Phoenix1, Phoenix2 };

struct IpVersion {
    uint8_t major, minor, revision;
};

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
// gc for GC, psp for MP0, sdma for SDMA0, vcn for VCN (UVD hw id). False if the IP is missing or out is too small.
bool fw_prefix(const IpDiscoveryParser &disc, HwId hw, char *out, size_t out_len);

} // namespace rdna3
