// phx_dcn_timing.h: the OTG timing registers for a display mode, computed as Linux optc1_program_timing does
// (drivers/gpu/drm/amd/display/dc/optc/dcn10/dcn10_optc.c; DCN 3.1.4's optc31 uses it). Pure computation: nothing is
// written. Phase 3b's first, read-only check: for the GOP's 1080p60 mode it must give exactly the registers GOP left on
// the 8500G (tests/test_phx_dcn_timing.cpp), so the 144/165/180 Hz values come from the same, proven formula.
//
// Registers: dcn_3_1_4_offset.h (OTG0, BASE_IDX 2, DMU segment 2 = 0x34c0; OTG n at +0x80 * n), fields from
// dcn_3_1_4_sh_mask.h.
#pragma once
#include <stdint.h>

namespace phxtiming {

// One mode as the EDID detailed timing descriptor gives it (and as DC's dc_crtc_timing holds it).
struct Mode {
    uint32_t pixelKhz;
    uint32_t hActive, hFrontPorch, hSync, hTotal;
    uint32_t vActive, vFrontPorch, vSync, vTotal;
    bool hSyncPositive, vSyncPositive;
    uint32_t refreshMilliHz() const {
        return hTotal && vTotal ? (uint32_t)((uint64_t)pixelKhz * 1000000ull / ((uint64_t)hTotal * vTotal)) : 0;
    }
};

// OTG0 dword offsets in DMU segment 2 (dcn_3_1_4_offset.h)
static constexpr uint32_t kOtgHTotal = 0x1b2a, kOtgHBlankStartEnd = 0x1b2b, kOtgHSyncA = 0x1b2c, kOtgHSyncACntl = 0x1b2d,
                          kOtgVTotal = 0x1b2f, kOtgVBlankStartEnd = 0x1b36, kOtgVSyncA = 0x1b37, kOtgVSyncACntl = 0x1b38;

struct OtgRegs {
    uint32_t hTotal, hBlankStartEnd, hSyncA, hSyncPol, vTotal, vBlankStartEnd, vSyncA, vSyncPol;
};

// optc1_program_timing, for a progressive mode without borders (none of the EDID modes here has either):
//   OTG_H_TOTAL = h_total - 1;  OTG_H_SYNC_A: START 0, END h_sync_width
//   OTG_H_BLANK_START = h_total - h_front_porch;  OTG_H_BLANK_END = blank_start - h_addressable
//   OTG_H_SYNC_A_POL = HSYNC_POSITIVE ? 0 : 1   (and the same for V, after apply_front_porch_workaround: vfp >= 1)
// Fields: START/BLANK_START bits 14:0, END bits 30:16, TOTAL bits 14:0, POL bit 0.
inline OtgRegs otgRegs(const Mode &m) {
    const uint32_t vfp = m.vFrontPorch < 1 ? 1 : m.vFrontPorch;   // apply_front_porch_workaround (progressive)
    OtgRegs r{};
    r.hTotal = (m.hTotal - 1) & 0x7fff;
    r.hSyncA = (m.hSync & 0x7fff) << 16;
    const uint32_t hbs = m.hTotal - m.hFrontPorch, hbe = hbs - m.hActive;
    r.hBlankStartEnd = (hbs & 0x7fff) | ((hbe & 0x7fff) << 16);
    r.hSyncPol = m.hSyncPositive ? 0 : 1;
    r.vTotal = (m.vTotal - 1) & 0x7fff;
    r.vSyncA = (m.vSync & 0x7fff) << 16;
    const uint32_t vbs = m.vTotal - vfp, vbe = vbs - m.vActive;
    r.vBlankStartEnd = (vbs & 0x7fff) | ((vbe & 0x7fff) << 16);
    r.vSyncPol = m.vSyncPositive ? 0 : 1;
    return r;
}

// An EDID 18-byte detailed timing descriptor (EDID 1.3/1.4, CEA-861 DTD) as a Mode; false for a non-timing descriptor.
inline bool modeFromDtd(const uint8_t d[18], Mode *m) {
    const uint32_t pc = (uint32_t)(d[0] | d[1] << 8) * 10;   // kHz
    if (!pc) return false;
    const uint32_t ha = d[2] | (uint32_t)(d[4] >> 4) << 8, hb = d[3] | (uint32_t)(d[4] & 15) << 8;
    const uint32_t va = d[5] | (uint32_t)(d[7] >> 4) << 8, vb = d[6] | (uint32_t)(d[7] & 15) << 8;
    const uint32_t hso = d[8] | (uint32_t)((d[11] >> 6) & 3) << 8, hsw = d[9] | (uint32_t)((d[11] >> 4) & 3) << 8;
    const uint32_t vso = (uint32_t)(d[10] >> 4) | (uint32_t)((d[11] >> 2) & 3) << 4;
    const uint32_t vsw = (uint32_t)(d[10] & 15) | (uint32_t)(d[11] & 3) << 4;
    m->pixelKhz = pc;
    m->hActive = ha; m->hFrontPorch = hso; m->hSync = hsw; m->hTotal = ha + hb;
    m->vActive = va; m->vFrontPorch = vso; m->vSync = vsw; m->vTotal = va + vb;
    const bool digitalSeparate = ((d[17] >> 3) & 3) == 3;
    m->hSyncPositive = digitalSeparate && (d[17] & 2);
    m->vSyncPositive = digitalSeparate && (d[17] & 4);
    return true;
}

// Every detailed timing in an EDID: block 0's four descriptors, then each CEA-861 extension's DTDs (byte 2 = DTD
// offset, 18-byte descriptors until a zero pixel clock or the checksum byte), as Linux drm_edid.c walks them
// (drm_for_each_detailed_block: the base block, then cea_db / cea_detailed_timings). Returns the count stored.
inline int modesFromEdid(const uint8_t *e, uint32_t len, Mode *out, int max) {
    int n = 0;
    if (len < 128) return 0;
    for (uint32_t o = 54; o + 18 <= 126 && n < max; o += 18)
        if (modeFromDtd(e + o, &out[n])) n++;
    const uint32_t blocks = len / 128;
    for (uint32_t b = 1; b < blocks && b <= e[126]; b++) {
        const uint8_t *x = e + 128 * b;
        if (x[0] != 0x02 || x[2] < 4) continue;   // CEA extension tag 0x02; byte 2 = 0..3: no DTDs
        for (uint32_t o = x[2]; o + 18 <= 127 && n < max; o += 18)
            if (!modeFromDtd(x + o, &out[n])) break; else n++;
    }
    return n;
}

// The live OTG timing registers of OTG n, masked to the fields otgRegs computes (so a diff means a real difference).
static constexpr uint32_t kDmuSeg2 = 0x34c0, kOtgStride = 0x80;
template <typename Rd> OtgRegs readOtg(Rd rd, uint32_t otg) {
    auto R = [&](uint32_t reg) { return rd(kDmuSeg2 + reg + otg * kOtgStride); };
    OtgRegs r{};
    r.hTotal = R(kOtgHTotal) & 0x7fff;
    r.hBlankStartEnd = R(kOtgHBlankStartEnd) & 0x7fff7fff;
    r.hSyncA = R(kOtgHSyncA) & 0x7fff7fff;
    r.hSyncPol = R(kOtgHSyncACntl) & 1;
    r.vTotal = R(kOtgVTotal) & 0x7fff;
    r.vBlankStartEnd = R(kOtgVBlankStartEnd) & 0x7fff7fff;
    r.vSyncA = R(kOtgVSyncA) & 0x7fff7fff;
    r.vSyncPol = R(kOtgVSyncACntl) & 1;
    return r;
}

// Which fields differ (bit i = field i in OtgRegs order); 0 = identical.
inline uint32_t otgDiff(const OtgRegs &a, const OtgRegs &b) {
    const uint32_t x[8] = {a.hTotal, a.hBlankStartEnd, a.hSyncA, a.hSyncPol, a.vTotal, a.vBlankStartEnd, a.vSyncA, a.vSyncPol};
    const uint32_t y[8] = {b.hTotal, b.hBlankStartEnd, b.hSyncA, b.hSyncPol, b.vTotal, b.vBlankStartEnd, b.vSyncA, b.vSyncPol};
    uint32_t m = 0;
    for (int i = 0; i < 8; i++) if (x[i] != y[i]) m |= 1u << i;
    return m;
}
static const char *const kOtgFieldNames[8] = {"OTG_H_TOTAL", "OTG_H_BLANK_START_END", "OTG_H_SYNC_A", "OTG_H_SYNC_A_POL",
                                              "OTG_V_TOTAL", "OTG_V_BLANK_START_END", "OTG_V_SYNC_A", "OTG_V_SYNC_A_POL"};
static const uint32_t kOtgFieldRegs[8] = {kOtgHTotal, kOtgHBlankStartEnd, kOtgHSyncA, kOtgHSyncACntl,
                                          kOtgVTotal, kOtgVBlankStartEnd, kOtgVSyncA, kOtgVSyncACntl};

} // namespace phxtiming
