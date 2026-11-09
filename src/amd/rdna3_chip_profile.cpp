//
//  rdna3_chip_profile.cpp — Phoenix / Phoenix 2 profiles keyed on the discovered IP versions.
//
#include "rdna3_chip_profile.h"

namespace rdna3 {

namespace {

constexpr ChipProfile kProfiles[] = {
    {
        PhxChip::Phoenix1, "Phoenix", {11, 0, 1}, {13, 0, 4}, {13, 0, 4}, {3, 0, 1}, {7, 7, 0},
        // psp_v13_0_4.c includes mp/mp_13_0_4_offset.h: regMP0_SMN_C2PMSG_*_BASE_IDX 1
        1,
        // smu_v13_0_4_ppt.c, smu_v13_0_4_init_msg_ctl: mmMP1_SMN_C2PMSG_66/82/90_BASE_IDX 1
        1,
        // soc21.c soc21_common_early_init, case IP_VERSION(11, 0, 1): external_rev_id = rev_id + 0x1
        0x1,
    },
    {
        PhxChip::Phoenix2, "Phoenix 2", {11, 0, 4}, {13, 0, 11}, {13, 0, 11}, {3, 0, 1}, {7, 7, 1},
        // amdgpu_psp.c psp_early_init: 13.0.11 -> psp_v13_0_set_psp_funcs; psp_v13_0.c includes
        // mp/mp_13_0_2_offset.h: regMP0_SMN_C2PMSG_*_BASE_IDX 0
        0,
        // smu_v13_0_4_ppt.c smu_v13_0_4_set_ppt_funcs: MP1 != 13.0.4 -> smu_v13_0_init_msg_ctl (smu_v13_0.c,
        // mp/mp_13_0_2_offset.h: regMP1_SMN_C2PMSG_66/82/90_BASE_IDX 0)
        0,
        // soc21.c soc21_common_early_init, case IP_VERSION(11, 0, 4): external_rev_id = rev_id + 0x80
        0x80,
    },
};

// Register dword offsets inside their IP segment. Identical in mp_13_0_4_offset.h and mp_13_0_2_offset.h; only the
// BASE_IDX differs (ChipProfile).
constexpr uint32_t kMp0C2pmsg58 = 0x007a, kMp0C2pmsg64 = 0x0080, kMp0C2pmsg67 = 0x0083, kMp0C2pmsg69 = 0x0085,
                   kMp0C2pmsg70 = 0x0086, kMp0C2pmsg71 = 0x0087, kMp0C2pmsg81 = 0x0091;
// SMU mailbox: msg = C2PMSG_66, arg = C2PMSG_82, resp = C2PMSG_90 (smu_v13_0_4_init_msg_ctl / smu_v13_0_init_msg_ctl)
constexpr uint32_t kMp1C2pmsg66 = 0x0282, kMp1C2pmsg82 = 0x0292, kMp1C2pmsg90 = 0x029a;
// gc/gc_11_0_0_offset.h (included by gfx_v11_0.c for 11.0.1 and 11.0.4): regGB_ADDR_CONFIG 0x13de, BASE_IDX 0
constexpr uint32_t kGcGbAddrConfig = 0x13de;
constexpr uint8_t kGcGbAddrConfigBaseIdx = 0;
// mmhub/mmhub_3_0_1_offset.h (gmc_v11_0_set_mmhub_funcs: MMHUB 3.0.1 -> mmhub_v3_0_1): BASE_IDX 1
constexpr uint32_t kMmhubFbLocationBase = 0x08ec, kMmhubFbLocationTop = 0x08ed, kMmhubFbOffset = 0x08d7;
constexpr uint8_t kMmhubFbBaseIdx = 1;
// nbio/nbio_7_7_0_offset.h (amdgpu_discovery.c: NBIO 7.7.0 and 7.7.1 -> nbio_v7_7_funcs, nbio_v7_7.c
// nbio_v7_7_remap_hdp_registers): regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL 0x012d, BASE_IDX 2
constexpr uint32_t kNbioRemapHdpMemFlushCntl = 0x012d;
constexpr uint8_t kNbioRemapBaseIdx = 2;

bool same(const IpEntry &e, const IpVersion &v) {
    return e.major == v.major && e.minor == v.minor && e.revision == v.revision;
}

bool reg(const IpDiscoveryParser &disc, HwId hw, uint8_t seg, uint32_t off, uint32_t &out) {
    uint32_t byte;
    if (!disc.reg_byte_offset(hw, 0, seg, off, byte))
        return false;
    out = byte / 4;
    return true;
}

} // namespace

const ChipProfile *chip_profile(const IpDiscoveryParser &disc) {
    IpEntry gc, mp0, mp1, mmhub, nbif;
    if (!disc.is_valid() || !disc.find_ip(HwId::Gc, 0, gc) || !disc.find_ip(HwId::Mp0, 0, mp0) ||
        !disc.find_ip(HwId::Mp1, 0, mp1) || !disc.find_ip(HwId::Mmhub, 0, mmhub) || !disc.find_ip(HwId::Nbif, 0, nbif))
        return nullptr;
    for (const ChipProfile &p : kProfiles)
        if (same(gc, p.gc) && same(mp0, p.mp0) && same(mp1, p.mp1) && same(mmhub, p.mmhub) && same(nbif, p.nbif))
            return &p;
    return nullptr;
}

bool chip_regs(const IpDiscoveryParser &disc, const ChipProfile &prof, ChipRegs &r) {
    const uint8_t ps = prof.psp_mbox_base_idx, ss = prof.smu_mbox_base_idx;
    return reg(disc, HwId::Mp0, ps, kMp0C2pmsg58, r.psp_c2pmsg_58) &&
           reg(disc, HwId::Mp0, ps, kMp0C2pmsg64, r.psp_c2pmsg_64) &&
           reg(disc, HwId::Mp0, ps, kMp0C2pmsg67, r.psp_c2pmsg_67) &&
           reg(disc, HwId::Mp0, ps, kMp0C2pmsg69, r.psp_c2pmsg_69) &&
           reg(disc, HwId::Mp0, ps, kMp0C2pmsg70, r.psp_c2pmsg_70) &&
           reg(disc, HwId::Mp0, ps, kMp0C2pmsg71, r.psp_c2pmsg_71) &&
           reg(disc, HwId::Mp0, ps, kMp0C2pmsg81, r.psp_c2pmsg_81) &&
           reg(disc, HwId::Mp1, ss, kMp1C2pmsg66, r.smu_msg) &&
           reg(disc, HwId::Mp1, ss, kMp1C2pmsg82, r.smu_arg) &&
           reg(disc, HwId::Mp1, ss, kMp1C2pmsg90, r.smu_resp) &&
           reg(disc, HwId::Gc, kGcGbAddrConfigBaseIdx, kGcGbAddrConfig, r.gb_addr_config) &&
           reg(disc, HwId::Mmhub, kMmhubFbBaseIdx, kMmhubFbLocationBase, r.mmhub_fb_location_base) &&
           reg(disc, HwId::Mmhub, kMmhubFbBaseIdx, kMmhubFbLocationTop, r.mmhub_fb_location_top) &&
           reg(disc, HwId::Mmhub, kMmhubFbBaseIdx, kMmhubFbOffset, r.mmhub_fb_offset) &&
           reg(disc, HwId::Nbif, kNbioRemapBaseIdx, kNbioRemapHdpMemFlushCntl, r.nbio_remap_hdp_mem_flush_cntl);
}

bool fw_prefix(const IpDiscoveryParser &disc, HwId hw, char *out, size_t out_len) {
    const char *ip;
    switch (hw) {
    case HwId::Gc: ip = "gc"; break;
    case HwId::Mp0: ip = "psp"; break;
    case HwId::Sdma0: ip = "sdma"; break;
    case HwId::Vcn: ip = "vcn"; break;
    default: return false;
    }
    IpEntry e;
    if (!out || !disc.find_ip(hw, 0, e))
        return false;
    // "<ip>_<maj>_<min>_<rev>": each number is at most 3 digits
    char buf[32];
    size_t n = 0;
    for (const char *c = ip; *c; c++)
        buf[n++] = *c;
    const uint8_t ver[3] = {e.major, e.minor, e.revision};   // no std::initializer_list in the kext
    for (uint8_t v : ver) {
        buf[n++] = '_';
        if (v >= 100) buf[n++] = static_cast<char>('0' + v / 100);
        if (v >= 10) buf[n++] = static_cast<char>('0' + v / 10 % 10);
        buf[n++] = static_cast<char>('0' + v % 10);
    }
    if (n + 1 > out_len)
        return false;
    for (size_t i = 0; i < n; i++)
        out[i] = buf[i];
    out[n] = '\0';
    return true;
}

} // namespace rdna3
