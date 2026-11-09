// phx_dcn314_dpms.h: Test L step 5b. Linux's DPMS off and on for the live DVI stream: the link and its encoders go
// down and come back up while the OTG keeps running (dc_commit_updates with stream_update->dpms_off ->
// link_set_dpms_off / link_set_dpms_on, Linux v6.17 link/link_dpms.c). Same mode, same 148.5 MHz pixel clock; the
// monitor loses the signal and gets it back. Pure sequence over register callbacks and the DMUB inbox
// (phx_dmub_inbox.h); tests/test_phx_dcn314_dpms.cpp checks the order over fakes.
//
// For SIGNAL_TYPE_DVI_SINGLE_LINK on DCN 3.1.4 (stream encoder DIG FE 0 = ENGINE_ID_DIGA, link encoder DIG BE 0 = UNIPHY
// A, OTG0), each call that does something:
//  off: link_set_dpms_off
//    - no AV mute, audio, SCDC or info packets (HDMI only); dce110_blank_stream acts on DP / eDP only
//    - dce110_disable_stream -> reset_dio_stream_encoder (link_hwss_dio.c): enc314_disable_fifo (DIG_FIFO_ENABLE 0),
//      enc314_set_dig_input_mode(0) (DIG_FIFO_OUTPUT_PIXEL_MODE 0), dcn10_link_encoder_connect_dig_be_to_fe(DIGA,
//      false) (DIG_FE_SOURCE_SELECT &= ~0x1). enc314 has no enable_stream, dccg314 no disable_symclk_se.
//    - disable_link -> dcn314_disable_link_output -> disable_dio_link_output -> dcn31_link_encoder_disable_output
//      (UNIPHY A is not a DPIA) -> dcn10_link_encoder_disable_output: only if DIG_BE_EN_CNTL.DIG_ENABLE, the DMUB
//      DIG1_TRANSMITTER_CONTROL disable. apply_symclk_on_tx_off_wa: symclk_ref_cnts.otg is only set for HDMI, nothing
//      for DVI. vpg31_powerdown: nothing (dcn314 leaves debug.enable_mem_low_power unset).
//  on: link_set_dpms_on
//    - dcn10_link_encoder_setup(DVI): DIG_BE_CNTL.DIG_MODE 2
//    - optc3_set_out_mux(OUT_MUX_DIO): OTG_CONTROL.OTG_OUT_MUX 0
//    - setup_dio_stream_attribute: enc1_setup_stereo_sync(OTG0, no 3D) (DIG_STEREOSYNC_SELECT 0, GATE_EN 1);
//      enc314_stream_encoder_dvi_set_stream_attribute: avoid_vbios_exec_table 0 -> DMUB DIGX_ENCODER_CONTROL
//      STREAM_SETUP (DVI, 148500 kHz, 4 lanes), then the helper: TMDS_PIXEL_ENCODING 0, TMDS_COLOR_FORMAT 0
//    - vpg31_poweron: nothing while VPG_GSP_MEM_PWR_STATE is 0 (checked before the run); update_info_frame: nothing
//      for DVI
//    - enable_link -> enable_link_hdmi (no SCDC for DVI): setup_dio_stream_encoder (connect_dig_be_to_fe true,
//      set_input_mode(dio_se_pix_per_cycle 0), enc314_enable_fifo), then dce110_enable_tmds_link_output ->
//      dcn10_link_encoder_enable_tmds_output: DMUB DIG1_TRANSMITTER_CONTROL enable (DVI, 148500 kHz, 4 lanes)
//    - set_test_pattern: optc314 has none
//    - dcn10_link_encoder_setup again ("workaround for the incorrect value being applied from transmitter control")
//    - dcn20_enable_stream: dccg314_set_pixel_rate_div(OTG0, DIV_BY_1, DIV_BY_4) (only when it differs),
//      setup_dio_stream_encoder again, dcn30_program_dmdata_engine (no dynamic metadata: DME_CONTROL
//      METADATA_ENGINE_EN 0, HDMI_METADATA_PACKET_ENABLE 0, DOLBY_VISION_EN 0), update_info_frame (nothing for DVI),
//      set_early_control (optc1_set_early_control: nothing)
// enc314_enable_fifo: DIG_FIFO_READ_START_LEVEL 7, enc314_reset_fifo(true), (false), DIG_FIFO_ENABLE 1; reset_fifo
// writes DIG_FIFO_RESET and, while DIG_SYMCLK_FE_ON, waits for DIG_FIFO_RESET_DONE to follow (10 us x 5000), else 10 us.
#pragma once
#include "phx_dcn314_step2.h"
#include "phx_dmub_inbox.h"
#include "phx_dmub_vbios_cmd.h"
#include "phx_dcn314_hdmi.h"

namespace phxdcn314 {

static constexpr uint32_t kDigFeSourceDigA = 0x1;   // DCN10_DIG_FE_SOURCE_SELECT_DIGA (get_frontend_source)
static constexpr uint32_t kDigModeDvi = 2, kDigModeHdmi = 3;
static constexpr uint32_t kRateDivBy1 = 0, kRateDivBy4 = 3;   // enum pixel_rate_div (dccg.h)

struct DpmsCtx {
    DpmsCtx(Io i, phxdmubinbox::Io d, phxdmubinbox::Inbox *in, phxdmubcmd::Signal s, uint32_t pix)
        : io(i), dio(d), ib(in), signal(s), pixKhz(pix) {}
    Io io;
    phxdmubinbox::Io dio;
    phxdmubinbox::Inbox *ib;
    phxdmubcmd::Signal signal;   // the stream's signal (DVI here)
    uint32_t pixKhz;             // phy_pix_clk = pix_clk_100hz / 10
    phxdmubinbox::Status dmub = phxdmubinbox::Status::Ok;   // the first DMUB failure, if any
    unsigned fifoResetTimeouts = 0;
    // HDMI only (phx_dcn314_hdmi.h): DDC1 for SCDC, the AVI InfoFrame, and what SCDC answered
    const ScdcIo *scdc = nullptr;
    phxavi::InfoPacket avi{};
    ScdcResult scdcOff, scdcOn;
    uint8_t scdcTmdsAfterOn = 0xff;
    bool vpgIdle = true;
};

inline void upd(const Io &io, const Fld &f, uint32_t v) { io.wr(io.ctx, f.addr, fieldSet(io.rd(io.ctx, f.addr), f, v)); }

inline void sendCmd(DpmsCtx &c, const uint8_t cmd[64]) {
    const phxdmubinbox::Status s = phxdmubinbox::send(c.dio, *c.ib, cmd);
    if (s != phxdmubinbox::Status::Ok && c.dmub == phxdmubinbox::Status::Ok) c.dmub = s;
}

// enc314_reset_fifo
inline void resetFifo(DpmsCtx &c, bool reset) {
    upd(c.io, kDigFifoReset, reset ? 1 : 0);
    if (fieldGet(c.io.rd(c.io.ctx, kDigFeSymclkOn.addr), kDigFeSymclkOn)) {
        for (unsigned i = 0; i < 5000; i++) {
            if (fieldGet(c.io.rd(c.io.ctx, kDigFifoResetDone.addr), kDigFifoResetDone) == (reset ? 1u : 0u)) return;
            c.io.delayUs(c.io.ctx, 10);
        }
        c.fifoResetTimeouts++;   // REG_WAIT only logs a timeout
    } else {
        c.io.delayUs(c.io.ctx, 10);
    }
}
inline void enableFifo(DpmsCtx &c) {   // enc314_enable_fifo
    upd(c.io, kDigFifoStartLvl, 0x7);
    resetFifo(c, true);
    resetFifo(c, false);
    upd(c.io, kDigFifoEnable, 1);
}
inline void connectBeToFe(DpmsCtx &c, bool connect) {   // dcn10_link_encoder_connect_dig_be_to_fe(ENGINE_ID_DIGA)
    uint32_t f = fieldGet(c.io.rd(c.io.ctx, kDigBeFeSrc.addr), kDigBeFeSrc);
    f = connect ? (f | kDigFeSourceDigA) : (f & ~kDigFeSourceDigA);
    upd(c.io, kDigBeFeSrc, f);
}
inline void setupStreamEncoder(DpmsCtx &c) {   // setup_dio_stream_encoder
    connectBeToFe(c, true);
    upd(c.io, kDigFifoPixMode, 0);   // set_input_mode(dio_se_pix_per_cycle 0)
    enableFifo(c);
}
inline void linkEncSetup(DpmsCtx &c) {   // dcn10_link_encoder_setup
    upd(c.io, kDigBeMode, c.signal == phxdmubcmd::Signal::Hdmi ? kDigModeHdmi : kDigModeDvi);
}

// dcn31_program_pix_clk for a TMDS signal (dce_clock_source.c): OTG0_PIXEL_RATE_CNTL.PIPE0_DTO_SRC_SEL 0, the DMUB
// SET_PIXEL_CLOCK, then dce112_program_pixel_clk_resync (8 bpc, no 4:2:0: PHYPLLA_DCCG_DEEP_COLOR_CNTL 0; DCN 3.1.4's
// clock-source mask has no double-rate field).
inline void programPixClk(DpmsCtx &c) {
    upd(c.io, kPipe0DtoSrcSel, 0);
    uint8_t cmd[64];
    phxdmubcmd::setPixelClock(cmd, c.signal, c.pixKhz);
    sendCmd(c, cmd);
    upd(c.io, kPhyPllADeepColor, 0);
}

inline bool isHdmi(const DpmsCtx &c) { return c.signal == phxdmubcmd::Signal::Hdmi; }

inline void dpmsOff(DpmsCtx &c) {
    if (isHdmi(c)) {
        setAvmute(c.io, true);                           // link_set_dpms_off: set_avmute (HDMI, no skip_avmute patch)
        // (no audio stream; blank_stream does nothing for HDMI) then SCDC back to legacy: "vbios only handles 165Mhz"
        if (c.scdc) c.scdcOff = writeScdc(*c.scdc, 165000, false);
        // dce110_disable_stream for HDMI
        stopHdmiInfoPackets(c.io);
        resetHdmiStreamAttribute(c.io);
    }
    // dce110_disable_stream -> reset_dio_stream_encoder
    upd(c.io, kDigFifoEnable, 0);
    upd(c.io, kDigFifoPixMode, 0);
    connectBeToFe(c, false);
    // disable_link -> ... dcn10_link_encoder_disable_output
    if (fieldGet(c.io.rd(c.io.ctx, kDigBeEnable.addr), kDigBeEnable)) {
        uint8_t cmd[64];
        phxdmubcmd::transmitterDisable(cmd, c.signal);
        sendCmd(c, cmd);
    }
    // apply_symclk_on_tx_off_wa (dcn314_hwseq.c): "for TMDS signal, OTG relies SYMCLK even if TX video output is off",
    // so the pixel clock is programmed again and the PHY keeps SYMCLK running with the TX off (SYMCLK_ON_TX_OFF).
    // Linux arms it through symclk_ref_cnts.otg, which dcn20_enable_stream_timing sets for HDMI only, because Linux
    // drives an HDMI sink as HDMI. GOP runs this sink as DVI, and Test L 5b showed the same dependency on DVI: without
    // it OTG0 stopped (0 frames in 3 s), DIG_FIFO_ERROR was set after the link came back and the picture did not
    // (2026-11-05, hardware/dumps/b650m_8500g/testl5b-1.txt). So it is applied for both TMDS signals.
    programPixClk(c);
}

inline void dpmsOn(DpmsCtx &c) {
    uint8_t cmd[64];
    linkEncSetup(c);
    upd(c.io, kOtgOutMux, 0);                            // OUT_MUX_DIO
    upd(c.io, kDigFeStereoSel, 0);                       // enc1_setup_stereo_sync(OTG0, false)
    upd(c.io, kDigFeStereoGate, 1);
    // dvi_set_stream_attribute (pix_clk_100hz / 10) or hdmi_set_stream_attribute (actual_pix_clk_khz): the DMUB
    // STREAM_SETUP, then enc1_stream_encoder_set_stream_attribute_helper, then for HDMI the HDMI engine
    phxdmubcmd::encoderStreamSetup(cmd, c.signal, c.pixKhz);
    sendCmd(c, cmd);
    upd(c.io, kDigFeTmdsPixEnc, 0);                      // RGB
    upd(c.io, kDigFeTmdsColFmt, 0);
    if (isHdmi(c)) {
        hdmiStreamAttributeRegs(c.io, c.pixKhz);
        c.vpgIdle = updateHdmiInfoPackets(c.io, c.avi) && c.vpgIdle;   // update_info_frame (vpg31_poweron: nothing)
    }
    // enable_link -> enable_link_hdmi: SCDC for HDMI, the stream encoder, the transmitter, read_scdc_data
    if (isHdmi(c) && c.scdc) c.scdcOn = writeScdc(*c.scdc, c.pixKhz, false);
    setupStreamEncoder(c);
    phxdmubcmd::transmitterEnable(cmd, c.signal, c.pixKhz);
    sendCmd(c, cmd);
    if (isHdmi(c) && c.scdc) c.scdcTmdsAfterOn = readScdcTmdsConfig(*c.scdc);
    linkEncSetup(c);                                     // the second setup
    // dcn20_enable_stream
    const uint32_t k1 = fieldGet(c.io.rd(c.io.ctx, kOtg0RateDivK1.addr), kOtg0RateDivK1);
    const uint32_t k2 = fieldGet(c.io.rd(c.io.ctx, kOtg0RateDivK2.addr), kOtg0RateDivK2);
    if (k1 != kRateDivBy1 || k2 != kRateDivBy4)
        c.io.wr(c.io.ctx, kOtg0RateDivK1.addr,
                fieldSet(fieldSet(c.io.rd(c.io.ctx, kOtg0RateDivK1.addr), kOtg0RateDivK1, kRateDivBy1), kOtg0RateDivK2, kRateDivBy4));
    setupStreamEncoder(c);
    upd(c.io, kDmeEngineEn, 0);                          // dcn30_program_dmdata_engine, no dynamic metadata
    upd(c.io, kHdmiMetaPktEn, 0);
    upd(c.io, kDigFeDolbyEn, 0);
    if (isHdmi(c)) c.vpgIdle = updateHdmiInfoPackets(c.io, c.avi) && c.vpgIdle;   // dcn20_enable_stream: update_info_frame
}

}  // namespace phxdcn314
