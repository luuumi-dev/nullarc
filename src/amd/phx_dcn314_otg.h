// phx_dcn314_otg.h: Test L step 5c. The OTG half of Linux's mode set for the live pipe (OTG0 -> OPP0 -> DIG FE 0):
// the back end reset that stops the OTG, and dcn20_enable_stream_timing that starts it again with a mode's timing
// (Linux v6.17 hwss/dcn20/dcn20_hwseq.c, optc/dcn314 + dcn31 + dcn10, mpc/dcn30, opp/dcn20 + dcn10, dccg/dcn314).
// Pure sequence over phx_dcn314_step2.h's Io; tests/test_phx_dcn314_otg.cpp checks it over a fake register file.
//
// down (dcn20_reset_back_end_for_pipe, after link_set_dpms_off):
//   optc314_disable_crtc: OTG_MASTER_EN 0, VTG0_ENABLE 0, REG_WAIT OTG_CLOCK_CONTROL.OTG_BUSY 0 (1 us x 100000)
//   optc1_enable_optc_clock(false): clear an OTG underflow first (OPTC_UNDERFLOW_OCCURRED_STATUS -> OPTC_UNDERFLOW_CLEAR),
//       OTG_CLOCK_GATE_DIS 0 + OTG_CLOCK_EN 0, OPTC_INPUT_CLK_GATE_DIS 0 + OPTC_INPUT_CLK_EN 0
//   optc314_set_odm_bypass: OPTC_DATA_SOURCE_SELECT (no segments, SEG0 = OTG inst, SEG1..3 = 0xf), OTG_H_TIMING_DIV_MODE
//       (no div: not two pixels per container), OPTC_MEMORY_CONFIG.OPTC_MEM_SEL 0
//   set_drr(NULL) -> optc31_set_drr: V_TOTAL_CONTROL MIN_MASK / MIN_SEL / MAX_SEL / FORCE_LOCK 0, V_TOTAL_MIN/MAX 0
//   SYMCLK_ON_TX_OFF -> disable_link_output again: dcn10_link_encoder_disable_output skips it (DIG_ENABLE is 0 by then)
// up (dcn20_enable_stream_timing):
//   dccg314_set_pixel_rate_div (unchanged for TMDS: k1 1, k2 4), optc1_enable_optc_clock(true) (OPTC_INPUT_CLK_EN +
//   GATE_DIS, wait OPTC_INPUT_CLK_ON (1 us x 1000), OTG_CLOCK_EN + GATE_DIS, wait OTG_CLOCK_ON), program_pix_clk
//   (phx_dcn314_dpms.h programPixClk), optc1_program_timing (below), mpc3_set_out_rate_control (MUX[0]: rate control
//   disabled, 0), opp1_pipe_clock_control(true), opp2_program_left_edge_extra_pixel (RGB: 0), blank_pixel_data(true)
//   (phx_dcn314_blank.h), optc314_enable_crtc (OPTC_SEG0_SRC_SEL, VTG0_ENABLE 1, OTG_DISABLE_POINT_CNTL 2 +
//   OTG_MASTER_EN 1), one frame's udelay, set_drr (no adjust: as above), optc1_set_static_screen_control(0, 2)
// optc1_program_timing, progressive, no borders: OTG_H_TOTAL, OTG_H_SYNC_A, OTG_H_BLANK_START_END, H sync polarity,
//   OTG_V_TOTAL, optc1_set_vtotal_min_max(v_total - 1, v_total - 1), OTG_V_SYNC_A, OTG_V_BLANK_START_END, V sync polarity
//   (the values phx_dcn_timing.h computes, which Test K matched against GOP's), OTG_INTERLACE_ENABLE 0, VTG0_ENABLE 0,
//   OTG_START_POINT_CNTL 0 + OTG_FIELD_NUMBER_CNTL 0 (TMDS, progressive), optc1_program_global_sync and
//   optc1_set_vtg_params(program_fp2) (phx_dcn314_step2.h), OPTC_DATA_FORMAT 0 (RGB), OTG_H_TIMING_DIV_MODE 0
// dcn314_resync_fifo_dccg_dio after the pipe is applied (dce110_apply_ctx_to_hw): no stream is dpms_off, so no OTG is
//   stopped; dccg314_trigger_dio_fifo_resync writes DENTIST_DISPCLK_WDIVIDER with DENTIST_DISPCLK_RDIVIDER
#pragma once
#include "phx_dcn314_step2.h"
#include "phx_dcn_timing.h"

namespace phxdcn314 {

struct OtgResult { bool busyCleared = true, inClkOn = true, otgClkOn = true; bool underflowCleared = false; };

inline void updF(const Io &io, const Fld &f, uint32_t v) { io.wr(io.ctx, f.addr, fieldSet(io.rd(io.ctx, f.addr), f, v)); }
inline void upd2(const Io &io, const Fld &a, uint32_t va, const Fld &b, uint32_t vb) {
    io.wr(io.ctx, a.addr, fieldSet(fieldSet(io.rd(io.ctx, a.addr), a, va), b, vb));
}
inline bool waitField(const Io &io, const Fld &f, uint32_t want, unsigned tries) {
    for (unsigned i = 0; i < tries; i++) {
        if (fieldGet(io.rd(io.ctx, f.addr), f) == want) return true;
        io.delayUs(io.ctx, 1);
    }
    return false;
}

inline void disableCrtc(const Io &io, OtgResult &r) {   // optc314_disable_crtc
    updF(io, kOtgMasterEn, 0);
    updF(io, kVtgEnable, 0);
    r.busyCleared = waitField(io, kOtgBusy, 0, 100000);
}
inline void enableOptcClock(const Io &io, bool en, OtgResult &r) {   // optc1_enable_optc_clock
    if (en) {
        upd2(io, kOptcInClkEn, 1, kOptcInClkGateDis, 1);
        r.inClkOn = waitField(io, kOptcInClkOn, 1, 1000);
        upd2(io, kOtgClockEn, 1, kOtgClockGateDis, 1);
        r.otgClkOn = waitField(io, kOtgClockOn, 1, 1000);
    } else {
        if (fieldGet(io.rd(io.ctx, kOptcUnderflow.addr), kOptcUnderflow) == 1) {
            updF(io, kOptcUnderflowClear, 1);
            r.underflowCleared = true;
        }
        upd2(io, kOtgClockGateDis, 0, kOtgClockEn, 0);
        upd2(io, kOptcInClkGateDis, 0, kOptcInClkEn, 0);
    }
}
inline void setOdmBypass(const Io &io) {   // optc314_set_odm_bypass, OTG0
    uint32_t w = fieldSet(0, kOdmNumSegments, 0);
    w = fieldSet(w, kOdmSeg0Src, 0);
    w = fieldSet(w, kOdmSeg1Src, 0xf);
    w = fieldSet(w, kOdmSeg2Src, 0xf);
    w = fieldSet(w, kOdmSeg3Src, 0xf);
    io.wr(io.ctx, kOdmSeg0Src.addr, w);
    updF(io, kOtgHDivMode, 0);   // H_TIMING_NO_DIV
    io.wr(io.ctx, kOptcMemSel.addr, fieldSet(0, kOptcMemSel, 0));
}
inline void setVtotalMinMax(const Io &io, uint32_t mn, uint32_t mx) {   // optc1_set_vtotal_min_max
    io.wr(io.ctx, kOtgVTotalMax.addr, fieldSet(0, kOtgVTotalMax, mx));
    io.wr(io.ctx, kOtgVTotalMin.addr, fieldSet(0, kOtgVTotalMin, mn));
}
inline void setDrrOff(const Io &io) {   // optc31_set_drr without DRR parameters
    uint32_t c = io.rd(io.ctx, kOtgVtMinMask.addr);
    c = fieldSet(c, kOtgVtMinMask, 0);
    c = fieldSet(c, kOtgVtMinSel, 0);
    c = fieldSet(c, kOtgVtMaxSel, 0);
    c = fieldSet(c, kOtgForceLock, 0);
    io.wr(io.ctx, kOtgVtMinMask.addr, c);
    setVtotalMinMax(io, 0, 0);
}

// optc1_program_timing for OTG0, the global sync and VTG parameters from the DML plan
inline bool programTiming(const Io &io, const phxtiming::Mode &m, const PhxDmlVal *plan) {
    using namespace phxtiming;
    const OtgRegs t = otgRegs(m);
    const uint32_t S = 0x34c0;   // DMU segment 2 (phx_dcn_timing.h offsets are segment-relative)
    auto upd = [&](uint32_t reg, uint32_t mask, uint32_t val) {
        io.wr(io.ctx, S + reg, (io.rd(io.ctx, S + reg) & ~mask) | (val & mask));
    };
    io.wr(io.ctx, S + kOtgHTotal, t.hTotal);                      // REG_SET(OTG_H_TOTAL)
    upd(kOtgHSyncA, 0x7fff7fff, t.hSyncA);                       // REG_UPDATE_2 START, END
    upd(kOtgHBlankStartEnd, 0x7fff7fff, t.hBlankStartEnd);
    upd(kOtgHSyncACntl, 0x1, t.hSyncPol);
    io.wr(io.ctx, S + kOtgVTotal, t.vTotal);                      // REG_SET(OTG_V_TOTAL)
    setVtotalMinMax(io, t.vTotal, t.vTotal);
    upd(kOtgVSyncA, 0x7fff7fff, t.vSyncA);
    upd(kOtgVBlankStartEnd, 0x7fff7fff, t.vBlankStartEnd);
    upd(kOtgVSyncACntl, 0x1, t.vSyncPol);
    updF(io, kOtgInterlace, 0);
    updF(io, kVtgEnable, 0);                                     // "VTG enable set to 0 first VInit"
    upd2(io, kOtgStartPoint, 0, kOtgFieldNumber, 0);
    // optc1_program_global_sync: the three OTG registers of the plan, REG_SET each
    for (unsigned r = 0; r < kNumProgRegs; r++) {
        const char *fn = kProgRegs[r].linuxFn, *g = "optc1_program_global_sync";
        while (*fn && *fn == *g) { fn++; g++; }
        if (*fn || *g) continue;
        uint32_t w; bool ovf;
        if (!regWord(r, plan, 0, &w, &ovf) || ovf) return false;
        io.wr(io.ctx, kProgRegs[r].addr, w);
    }
    unsigned vs, fp2, vinit;
    if (!phxDmlGet(plan, "dest.vstartup_start", &vs)) return false;
    vtgParams(VTiming{m.vTotal, m.vFrontPorch, m.vActive}, vs, &fp2, &vinit);
    upd2(io, kVtgFp2, fp2, kVtgVcountInit, vinit);
    updF(io, kOptcDataFormat, 0);                                // RGB
    updF(io, kOtgHDivMode, 0);
    return true;
}

inline void enableCrtc(const Io &io) {   // optc314_enable_crtc, OTG0
    updF(io, kOdmSeg0Src, 0);
    updF(io, kVtgEnable, 1);
    upd2(io, kOtgDisablePoint, 2, kOtgMasterEn, 1);
}
inline void outputPathOn(const Io &io) {   // mpc3_set_out_rate_control, opp1_pipe_clock_control, left edge (RGB)
    upd2(io, kMpcRateCtlDis, 1, kMpcRateCtl, 0);
    updF(io, kOppPipeClockEn, 1);
    updF(io, kFmtLeftEdge, 0);
}
inline void staticScreen(const Io &io, uint32_t events, uint32_t frames) {   // optc1_set_static_screen_control
    io.wr(io.ctx, kOtgStaticMask.addr, fieldSet(fieldSet(0, kOtgStaticMask, events), kOtgStaticFrames, frames > 0xff ? 0xff : frames));
}
inline void resyncDioFifo(const Io &io) {   // dccg314_trigger_dio_fifo_resync
    const uint32_t rdiv = fieldGet(io.rd(io.ctx, kDentistDispRdiv.addr), kDentistDispRdiv);
    updF(io, kDentistDispWdiv, rdiv);
}

// The back end reset after link_set_dpms_off
inline void otgDown(const Io &io, OtgResult &r) {
    disableCrtc(io, r);
    enableOptcClock(io, false, r);
    setOdmBypass(io);
    setDrrOff(io);
}

}  // namespace phxdcn314
