// phx_dcn314_step2.h: Test L step 2. Reprogram the live pipe (OTG0 / HUBP0) with Linux's DML values for the mode it
// already shows, in the order DCN 3.1.4 does it for a pipe update, and put the saved registers back. No clock, PLL,
// PHY or timing change. Pure sequence over rd / wr / delayUs callbacks (tools/phxctl/testl2.cpp runs it over
// PhxBringup's debug register selectors; tests/test_phx_dcn314_step2.cpp over a fake register file).
//
// Linux v6.17's order, dcn314_init.c -> dcn20_program_front_end_for_ctx / dcn20_program_pipe:
//   dcn20_pipe_control_lock -> optc3_lock (dcn30_optc.c): OTG_GLOBAL_CONTROL2.OTG_MASTER_UPDATE_LOCK_SEL = inst,
//       OTG_MASTER_UPDATE_LOCK = 1, REG_WAIT UPDATE_LOCK_STATUS == 1 (1 us x 10). HDMI uses no DMUB lock
//       (should_use_dmub_lock is for PSR / replay links).
//   dcn20_program_tg: optc1_program_global_sync, optc1_wait_for_state(VACTIVE) (OTG_V_ACTIVE_DISP, 1 us x 100000),
//       optc1_set_vtg_params(program_fp2 = true)
//   dcn20_update_dchubp_dpp: hubp2_vtg_sel, hubp3_setup, hubp2_setup_interdependent
//   dcn20_pipe_control_lock(false) -> optc1_unlock: OTG_MASTER_UPDATE_LOCK = 0
// Not in this step (each its own later step): the DET / compressed-buffer split and unbounded requesting
// (dcn31_program_compbuf_size in dcn20_optimize_bandwidth, hubp31_set_unbounded_requesting), the watermarks
// (hubbub31_program_watermarks), and HUBP_TTU_DISABLE, which GOP leaves at 1 (hubp2_set_blank clears it).
#pragma once
#include "phx_dcn314_plan.h"
#include <stdint.h>

namespace phxdcn314 {

struct Io {
    uint32_t (*rd)(void *ctx, uint32_t reg);
    void (*wr)(void *ctx, uint32_t reg, uint32_t val);
    void (*delayUs)(void *ctx, unsigned us);
    void *ctx;
};

// The stream's vertical timing, for optc1_set_vtg_params (the EDID mode the OTG already runs)
struct VTiming { unsigned vTotal, vFrontPorch, vActive; };

static constexpr unsigned kMaxWrites = 64;
struct Write { uint32_t addr, val; };
struct Plan {
    Write w[kMaxWrites];
    unsigned n = 0;
    unsigned firstAfterVactive = 0;   // writes [0, firstAfterVactive) are the global sync; then wait for VACTIVE
    bool ok = false;
};

enum class Result { Ok, LockTimeout, VactiveTimeout, BadPlan };

inline uint32_t fieldSet(uint32_t word, const Fld &f, uint32_t v) { return (word & ~f.mask) | ((v << f.shift) & f.mask); }
inline uint32_t fieldGet(uint32_t word, const Fld &f) { return (word & f.mask) >> f.shift; }

// optc1_set_vtg_params with apply_front_porch_workaround (progressive: front porch at least 1), no borders:
//   v_init = v_total - v_front_porch, asic_blank_end = v_init - v_addressable,
//   vertical_line_start = asic_blank_end - vstartup_start + 1, v_fp2 = -vertical_line_start when negative
inline void vtgParams(const VTiming &t, unsigned vstartup, unsigned *fp2, unsigned *vinit) {
    const unsigned vfp = t.vFrontPorch < 1 ? 1 : t.vFrontPorch;
    const int vInit = (int)t.vTotal - (int)vfp;
    const int blankEnd = vInit - (int)t.vActive;
    const int lineStart = blankEnd - (int)vstartup + 1;
    *fp2 = lineStart < 0 ? (unsigned)-lineStart : 0;
    *vinit = (unsigned)vInit;
}

// Every register word the step writes, in Linux's order, composed against the live values (REG_UPDATEs on a register
// written twice see the first write: DCHUBP_CNTL gets hubp2_vtg_sel, then hubp2_vready_at_or_After_vsync).
inline Plan buildPlan(const Io &io, const PhxDmlVal *p, const VTiming &t) {
    Plan out;
    struct Cur { uint32_t addr, val; } cur[kMaxWrites];
    unsigned nc = 0;
    auto current = [&](uint32_t addr) -> uint32_t & {
        for (unsigned i = 0; i < nc; i++) if (cur[i].addr == addr) return cur[i].val;
        cur[nc] = {addr, io.rd(io.ctx, addr)};
        return cur[nc++].val;
    };
    auto push = [&](uint32_t addr, uint32_t val) {
        if (out.n < kMaxWrites) out.w[out.n++] = {addr, val};
        current(addr) = val;
    };
    auto progReg = [&](unsigned r) -> bool {
        uint32_t w; bool ovf;
        if (!regWord(r, p, current(kProgRegs[r].addr), &w, &ovf) || ovf) return false;
        push(kProgRegs[r].addr, w);
        return true;
    };
    auto isFn = [](unsigned r, const char *fn) {
        const char *a = kProgRegs[r].linuxFn, *b = fn;
        while (*a && *a == *b) { a++; b++; }
        return !*a && !*b;
    };
    // dcn20_program_tg: program_global_sync
    for (unsigned r = 0; r < kNumProgRegs; r++)
        if (isFn(r, "optc1_program_global_sync") && !progReg(r)) return out;
    out.firstAfterVactive = out.n;
    // set_vtg_params (REG_UPDATE_2 CONTROL, VTG0_FP2 / VTG0_VCOUNT_INIT)
    unsigned vs, fp2, vinit;
    if (!phxDmlGet(p, "dest.vstartup_start", &vs)) return out;
    vtgParams(t, vs, &fp2, &vinit);
    push(kVtgFp2.addr, fieldSet(fieldSet(current(kVtgFp2.addr), kVtgFp2, fp2), kVtgVcountInit, vinit));
    // dcn20_update_dchubp_dpp: hubp2_vtg_sel(OTG0), hubp3_setup, hubp2_setup_interdependent (the table's order)
    push(kHubpVtgSel.addr, fieldSet(current(kHubpVtgSel.addr), kHubpVtgSel, 0));
    for (unsigned r = 0; r < kNumProgRegs; r++)
        if (!isFn(r, "optc1_program_global_sync") && !progReg(r)) return out;
    out.ok = out.n < kMaxWrites;
    return out;
}

// The registers buildPlan writes, as they are now: what restore() puts back
struct Saved { Write w[kMaxWrites]; unsigned n = 0; };
inline Saved save(const Io &io, const Plan &pl) {
    Saved s;
    for (unsigned i = 0; i < pl.n; i++) {
        bool seen = false;
        for (unsigned j = 0; j < s.n; j++) seen |= s.w[j].addr == pl.w[i].addr;
        if (!seen) s.w[s.n++] = {pl.w[i].addr, io.rd(io.ctx, pl.w[i].addr)};
    }
    return s;
}

// optc3_lock for OTG0: false when UPDATE_LOCK_STATUS did not come up in 10 x 1 us (then the lock is released again
// and nothing else is written; Linux carries on after a REG_WAIT timeout, this step does not).
inline bool lock(const Io &io) {
    io.wr(io.ctx, kOtgLockSel.addr, fieldSet(io.rd(io.ctx, kOtgLockSel.addr), kOtgLockSel, 0));
    io.wr(io.ctx, kOtgLock.addr, fieldSet(0, kOtgLock, 1));
    for (unsigned i = 0; i < 10; i++) {
        if (fieldGet(io.rd(io.ctx, kOtgLockStatus.addr), kOtgLockStatus)) return true;
        io.delayUs(io.ctx, 1);
    }
    io.wr(io.ctx, kOtgLock.addr, fieldSet(0, kOtgLock, 0));
    return false;
}
inline void unlock(const Io &io) { io.wr(io.ctx, kOtgLock.addr, fieldSet(0, kOtgLock, 0)); }

inline bool waitVactive(const Io &io) {
    for (unsigned i = 0; i < 100000; i++) {
        if (fieldGet(io.rd(io.ctx, kOtgVActiveDisp.addr), kOtgVActiveDisp)) return true;
        io.delayUs(io.ctx, 1);
    }
    return false;
}

// Lock, the writes (waiting for VACTIVE after the global sync, as dcn20_program_tg does), unlock. A VACTIVE timeout
// still writes the rest and unlocks, as Linux does (REG_WAIT only logs), and is reported.
inline Result apply(const Io &io, const Write *w, unsigned n, unsigned firstAfterVactive) {
    if (!lock(io)) return Result::LockTimeout;
    bool vact = true;
    for (unsigned i = 0; i < n; i++) {
        if (i == firstAfterVactive) vact = waitVactive(io);
        io.wr(io.ctx, w[i].addr, w[i].val);
    }
    unlock(io);
    return vact ? Result::Ok : Result::VactiveTimeout;
}
inline Result applyPlan(const Io &io, const Plan &pl) {
    return pl.ok ? apply(io, pl.w, pl.n, pl.firstAfterVactive) : Result::BadPlan;
}
// The saved words, under the same lock; the global sync registers first, as in the plan.
inline Result restore(const Io &io, const Saved &s, unsigned firstAfterVactive) {
    return apply(io, s.w, s.n, firstAfterVactive);
}

}  // namespace phxdcn314
