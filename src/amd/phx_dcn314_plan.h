// phx_dcn314_plan.h: turns a DML plan (phx_dml314_plans.h) into the HUBP0 / OTG0 register words Linux would write on
// DCN 3.1.4 (phx_dcn314_prog_regs.h). Pure computation, no MMIO: Test L's dry run (tools/phxctl/testl.cpp) compares
// these words with the live registers, the later mode switch writes them.
#pragma once
#include "phx_dcn314_prog_regs.h"
#include "phx_dml314_plans.h"
#include <stdint.h>

namespace phxdcn314 {

// hubbub31_get_dchub_ref_freq (dcn31_hubbub.c): dc_refclk 24 MHz, halved when the global timer runs with REFDIV 2;
// a stopped timer is a Linux ASSERT_CRITICAL and leaves 24 MHz.
inline unsigned dchubRefKhz(uint32_t timerCntl) {
    const unsigned div = timerCntl & 0xF, en = (timerCntl >> 12) & 1;   // DCHUBBUB_GLOBAL_TIMER_REFDIV / _ENABLE
    return en && div == 2 ? 12000 : 24000;
}

inline const PhxDmlVal *planFor(unsigned refreshHz, unsigned refKhz) {
    if (refreshHz == 144) return refKhz == 12000 ? kDml144Refdiv2 : kDml144;
    return refKhz == 12000 ? kDml60Refdiv2 : kDml60;
}

// hubp2_vready_at_or_After_vsync (dcn20_hubp.c), with pipe_dest's unsigned int arithmetic:
//   vstartup_start - (vready_offset + vupdate_width + vupdate_offset) / htotal <= vblank_end -> 1, else 0; htotal 0 -> 0
inline bool vreadyAfterVsync(const PhxDmlVal *p, unsigned *out) {
    unsigned vs, vr, vw, vo, ht, vbe;
    if (!phxDmlGet(p, "dest.vstartup_start", &vs) || !phxDmlGet(p, "dest.vready_offset", &vr) ||
        !phxDmlGet(p, "dest.vupdate_width", &vw) || !phxDmlGet(p, "dest.vupdate_offset", &vo) ||
        !phxDmlGet(p, "dest.htotal", &ht) || !phxDmlGet(p, "dest.vblank_end", &vbe))
        return false;
    *out = ht ? ((unsigned)(vs - (vr + vw + vo) / ht) <= vbe ? 1u : 0u) : 0u;
    return true;
}

// The value one field gets from the plan. false: the plan lacks it.
inline bool fieldValue(const ProgField &f, const PhxDmlVal *p, unsigned *v) {
    const char *s = f.dml;
    if (s[0] == 'c' && s[1] == 'o' && s[2] == 'n' && s[3] == 's' && s[4] == 't' && s[5] == ':') {
        unsigned x = 0;
        for (s += 6; *s >= '0' && *s <= '9'; s++) x = x * 10 + (unsigned)(*s - '0');
        *v = x;
        return true;
    }
    const char *c = "calc.vready_after_vsync", *t = s;
    while (*c && *c == *t) { c++; t++; }
    if (!*c && !*t) return vreadyAfterVsync(p, v);
    return phxDmlGet(p, s, v);
}

// The word Linux writes to register r: REG_SET / REG_WRITE start from 0, REG_UPDATE from the live value with only the
// named fields replaced. *overflow: some plan value had bits outside its field (REG_SET_n would drop them).
inline bool regWord(unsigned r, const PhxDmlVal *p, uint32_t live, uint32_t *word, bool *overflow) {
    if (r >= kNumProgRegs) return false;
    const ProgReg &R = kProgRegs[r];
    uint32_t w = R.kind == kUpdate ? live : 0, touched = 0;
    *overflow = false;
    for (unsigned i = R.firstField; i < kNumProgFields && kProgFields[i].reg == r; i++) {
        const ProgField &f = kProgFields[i];
        unsigned v;
        if (!fieldValue(f, p, &v)) return false;
        const uint64_t placed = (uint64_t)v << f.shift;
        if (placed & ~(uint64_t)f.mask) *overflow = true;
        w = (w & ~f.mask) | ((uint32_t)placed & f.mask);
        touched |= f.mask;
    }
    *word = w;
    return touched != 0;
}

}  // namespace phxdcn314
