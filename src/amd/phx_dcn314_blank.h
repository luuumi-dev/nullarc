// phx_dcn314_blank.h: Test L step 5a. dcn20_blank_pixel_data (Linux v6.17 hwss/dcn20/dcn20_hwseq.c) for one
// unsplit HDMI/DVI pipe: on DCN 3.1.4 an HDMI or DVI stream is blanked by its OPP's display pattern generator.
// dce110_blank_stream / dcn20_unblank_stream only touch DP and eDP streams. Linux blanks this way in
// dcn20_enable_stream_timing (before the OTG starts) and unblanks it from dcn20_program_pipe once the plane is visible,
// so the 144 Hz switch uses exactly these writes.
//   blank:   SOLID_COLOR (debug.visual_confirm unset): opp2_set_disp_pattern_generator writes DPG_DIMENSIONS and
//            DPG_OFFSET_SEGMENT, opp2_dpg_set_blank_color writes the three colour registers (black for an sRGB stream:
//            color_space_to_black_color, RGB full range = 0,0,0, each << 6), then REG_UPDATE_2 DPG_CONTROL DPG_EN 1,
//            DPG_MODE TEST_PATTERN_MODE_HORIZONTALBARS (4, hw_shared.h), then DPG_DIMENSIONS again
//   unblank: VIDEOMODE: DPG_DIMENSIONS, DPG_OFFSET_SEGMENT, then DPG_CONTROL, the colours and DPG_RAMP_CONTROL to 0
// The ODM slice is the whole stream (no ODM combine): width/height = the timing's active size, offset 0.
// No ABM on an HDMI stream (set_abm_immediate_disable / set_pipe do nothing without stream_res.abm).
#pragma once
#include "phx_dcn314_step2.h"

namespace phxdcn314 {

static constexpr uint32_t kDpgModeHorizontalBars = 4;   // TEST_PATTERN_MODE_HORIZONTALBARS

inline unsigned blankWrites(unsigned width, unsigned height, Write *w) {
    unsigned n = 0;
    const uint32_t dims = fieldSet(fieldSet(0, kDpgWidth, width), kDpgHeight, height);
    w[n++] = {kDpgWidth.addr, dims};
    w[n++] = {kDpgXOffset.addr, 0};   // DPG_X_OFFSET 0, DPG_SEGMENT_WIDTH 0
    const uint32_t black = 0;         // RGB full range black, << 6
    w[n++] = {kDpgColB1.addr, fieldSet(fieldSet(0, kDpgColB1, black << 6), kDpgColB0, black << 6)};
    w[n++] = {kDpgColG1.addr, fieldSet(fieldSet(0, kDpgColG1, black << 6), kDpgColG0, black << 6)};
    w[n++] = {kDpgColR1.addr, fieldSet(fieldSet(0, kDpgColR1, black << 6), kDpgColR0, black << 6)};
    w[n++] = {kDpgEn.addr, 0};        // placeholder: the REG_UPDATE of DPG_CONTROL is composed by applyBlank
    w[n++] = {kDpgWidth.addr, dims};
    return n;
}

inline void applyBlank(const Io &io, unsigned width, unsigned height) {
    Write w[8];
    const unsigned n = blankWrites(width, height, w);
    for (unsigned i = 0; i < n; i++) {
        if (w[i].addr == kDpgEn.addr)
            w[i].val = fieldSet(fieldSet(io.rd(io.ctx, kDpgEn.addr), kDpgEn, 1), kDpgMode, kDpgModeHorizontalBars);
        io.wr(io.ctx, w[i].addr, w[i].val);
    }
}

inline void applyUnblank(const Io &io, unsigned width, unsigned height) {
    io.wr(io.ctx, kDpgWidth.addr, fieldSet(fieldSet(0, kDpgWidth, width), kDpgHeight, height));
    io.wr(io.ctx, kDpgXOffset.addr, 0);
    io.wr(io.ctx, kDpgEn.addr, 0);       // REG_WRITE(DPG_CONTROL, 0)
    io.wr(io.ctx, kDpgColR1.addr, 0);
    io.wr(io.ctx, kDpgColG1.addr, 0);
    io.wr(io.ctx, kDpgColB1.addr, 0);
    io.wr(io.ctx, kDpgRamp0.addr, 0);    // REG_WRITE(DPG_RAMP_CONTROL, 0)
}

inline bool isBlanked(const Io &io) {   // opp2_dpg_is_blanked: DPG_EN and the solid-colour mode
    const uint32_t c = io.rd(io.ctx, kDpgEn.addr);
    return fieldGet(c, kDpgEn) && fieldGet(c, kDpgMode) == kDpgModeHorizontalBars;
}

}  // namespace phxdcn314
