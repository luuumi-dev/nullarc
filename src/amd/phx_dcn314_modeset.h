// phx_dcn314_modeset.h: Linux's mode set for the one HDMI/DVI pipe (OTG0, OPP0, HUBP0, DIG FE 0 / BE 0, PHY A), composed
// from the pieces Test L proved one by one, in dc_commit_streams' order (Linux v6.17):
//   link_set_dpms_off (phx_dcn314_dpms.h, 5b, with the TMDS SYMCLK workaround)
//   dcn20_reset_back_end_for_pipe (phx_dcn314_otg.h otgDown)
//   dce110_apply_single_controller_ctx_to_hw:
//     dcn20_enable_stream_timing: OTG / OPTC clocks on, program_pix_clk, optc1_program_timing, MPC / OPP output path,
//       blank_pixel_data(true) (phx_dcn314_blank.h, 5a), optc314_enable_crtc, one frame's wait, set_drr,
//       set_static_screen_control(0, 2)
//     set_drr and set_static_screen_control again, enc1_dig_connect_to_otg, link_set_dpms_on (5b)
//   dcn314_resync_fifo_dccg_dio (dce110_apply_ctx_to_hw, after the pipe)
//   the front end: hubp3_setup / hubp2_setup_interdependent with the global sync under the OTG lock (phx_dcn314_step2.h,
//     step 2), then blank_pixel_data(false) once the plane shows (dcn20_program_pipe)
// Not written, because GOP's values already are Linux's for an 8 bpc RGB stream on this pipe, and each is its own
// later check: opp_program_fmt / opp_set_dyn_expansion (FMT), the DPP / MPC blending of the plane, the DET split and
// watermarks (prepare / optimize bandwidth), the clocks (step 3, which a mode change raises first).
#pragma once
#include "phx_dcn314_blank.h"
#include "phx_dcn314_dpms.h"
#include "phx_dcn314_otg.h"

namespace phxdcn314 {

struct ModesetResult {
    OtgResult otg;
    bool timingOk = false;
    Result frontEnd = Result::BadPlan;
};

// dcn20_enable_stream_timing's wait: udelay(v_total * (h_total * 10000u / pix_clk_100hz)) microseconds
inline unsigned frameWaitUs(const phxtiming::Mode &m) {
    const unsigned pix100hz = m.pixelKhz * 10;
    return pix100hz ? m.vTotal * (m.hTotal * 10000u / pix100hz) : 0;
}

// c.signal / c.pixKhz describe the stream that is up now and comes down; `to` / m.pixelKhz the one that comes up (for
// HDMI, c.avi holds its AVI InfoFrame and c.scdc the DDC for SCDC)
inline ModesetResult modeset(DpmsCtx &c, const phxtiming::Mode &m, const PhxDmlVal *plan, phxdmubcmd::Signal to) {
    ModesetResult r;
    const Io &io = c.io;
    // down, as the old stream
    dpmsOff(c);
    otgDown(io, r.otg);
    // up, as the new stream
    c.signal = to;
    c.pixKhz = m.pixelKhz;
    // up: dcn20_enable_stream_timing
    enableOptcClock(io, true, r.otg);
    programPixClk(c);
    r.timingOk = programTiming(io, m, plan);
    outputPathOn(io);
    applyBlank(io, m.hActive, m.vActive);
    enableCrtc(io);
    io.delayUs(io.ctx, frameWaitUs(m));
    setDrrOff(io);
    staticScreen(io, 0, 2);
    // the rest of dce110_apply_single_controller_ctx_to_hw
    setDrrOff(io);
    staticScreen(io, 0, 2);
    updF(io, kDigFeSourceSel, 0);   // enc1_dig_connect_to_otg(OTG0)
    dpmsOn(c);
    resyncDioFifo(io);
    // the front end, then the unblank
    const Plan pl = buildPlan(io, plan, VTiming{m.vTotal, m.vFrontPorch, m.vActive});
    r.frontEnd = applyPlan(io, pl);
    applyUnblank(io, m.hActive, m.vActive);
    return r;
}

}  // namespace phxdcn314
