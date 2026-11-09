// phx_dmub_vbios_cmd.h: the DMUB_CMD__VBIOS ring commands Linux sends DMCUB to switch the PHY and its PLL on a
// DCN 3.1.4 HDMI/DVI link. Pure computation (64-byte inbox1 commands); nothing is sent here. Phase 3b, Tests L/M.
//
// Why these commands: the 8500G's VBIOS has no digxencodercontrol / setpixelclock / dig1transmittercontrol tables
// (docs/PHASE3B_PLAN.md 4e), so command_table2.c's fallbacks run, and with dmub_command_table set they go to DMCUB:
//   encoder_control_fallback     -> encoder_control_digx_v1_5 -> encoder_control_dmcub
//   transmitter_control_fallback -> transmitter_control_v1_7  -> transmitter_control_dmcub_v1_7
//   set_pixel_clock_fallback     -> set_pixel_clock_v7        -> set_pixel_clock_dmcub
// (Linux v6.17 drivers/gpu/drm/amd/display/dc/bios/command_table2.c; dcn314_resource.c debug_defaults sets
// dmub_command_table and leaves avoid_vbios_exec_table 0, so enc314_stream_encoder_hdmi_set_stream_attribute sends
// the encoder STREAM_SETUP command instead of its register-only branch).
//
// Layouts: dmub/inc/dmub_cmd.h (struct dmub_cmd_header, dmub_dig_transmitter_control_data_v1_7,
// dmub_rb_cmd_set_pixel_clock) and include/atomfirmware.h (set_pixel_clock_parameter_v1_7). Field values:
// command_table_helper2_dce112.c / command_table_helper.c conversions, filled as these callers fill them:
//   - PHY on:  dcn10_link_encoder_enable_tmds_output (DCN 3.1 link encoder funcs, dcn31_dio_link_encoder.c), from
//              enable_link_hdmi (link/link_dpms.c) with pixel_clock = stream->phy_pix_clk (kHz)
//   - PHY off: dcn10_link_encoder_disable_output (via dcn31_link_encoder_disable_output, non-DPIA link)
//   - PLL:     dcn31_program_pix_clk (dc/dce/dce_clock_source.c; dcn314_resource.c creates PLL0..4 with
//              dcn31_clk_src_construct), pixel rate from dcn3_get_pix_clk_dividers
// The 8500G's HDMI port (VBIOS displayobjectinfo path 0 + gpio_pin_lut, docs/PHASE3B_PLAN.md 4f): UNIPHY A
// (encoder object 0x211E), connector object 0x340C (id 0x0C HDMI type A), HPD1. One display: the first free PLL,
// pool->clock_sources[0] = CLOCK_SOURCE_COMBO_PHY_PLL0 (dc_resource_find_first_free_pll), OTG0 = CONTROLLER_ID_D0.
#pragma once
#include <stdint.h>
#include <string.h>

namespace phxdmubcmd {

// dmub_cmd.h
static constexpr uint8_t kCmdVbios = 128;                 // DMUB_CMD__VBIOS
static constexpr uint8_t kSubDigxEncoderControl = 0;     // DMUB_CMD__VBIOS_DIGX_ENCODER_CONTROL
static constexpr uint8_t kSubDig1TransmitterControl = 1;  // DMUB_CMD__VBIOS_DIG1_TRANSMITTER_CONTROL
static constexpr uint8_t kSubSetPixelClock = 2;           // DMUB_CMD__VBIOS_SET_PIXEL_CLOCK
static constexpr uint32_t kCmdBytes = 64;                 // union dmub_rb_cmd (DMUB_RB_CMD_SIZE)
// payload_bytes = sizeof(cmd) - sizeof(header): dig1_transmitter_control = 4 + 60 (v1_7 is the larger union member),
// set_pixel_clock = 4 + sizeof(set_pixel_clock_parameter_v1_7) 16
// digx_encoder_control = 4 + sizeof(union dig_encoder_control_parameters_v1_5) 12
static constexpr uint8_t kPayloadTransmitter = 60, kPayloadPixelClock = 16, kPayloadEncoder = 12;
static constexpr uint8_t kEncoderCmdStreamSetup = 0x0F;   // ATOM_ENCODER_CMD_STREAM_SETUP (encoder_action_to_atom, SETUP)

// atomfirmware.h / atombios.h
static constexpr uint8_t kPhyUniphyA = 0;            // ATOM_PHY_ID_UNIPHYA (phy_id_to_atom, TRANSMITTER_UNIPHY_A)
static constexpr uint8_t kActionDisable = 0;         // TRANSMITTER_CONTROL_DISABLE = ATOM_TRANSMITTER_ACTION_DISABLE
static constexpr uint8_t kActionEnable = 1;          // TRANSMITTER_CONTROL_ENABLE = ATOM_TRANSMITTER_ACTION_ENABLE
static constexpr uint8_t kDigModeDvi = 2;            // ATOM_TRANSMITTER_DIGMODE_V6_DVI / ATOM_ENCODER_MODE_DVI
static constexpr uint8_t kDigModeHdmi = 3;           // ATOM_TRANSMITTER_DIGMODE_V6_HDMI / ATOM_ENCODER_MODE_HDMI
static constexpr uint8_t kHpd1 = 1;                  // ATOM_TRANSMITTER_V6_HPD1_SEL (hpd_sel_to_atom, HPD_SOURCEID1)
static constexpr uint8_t kConnHdmiA = 0x0C;          // CONNECTOR_OBJECT_ID_HDMI_TYPE_A (connector 0x340C's id)
static constexpr uint8_t kComboPhyPll0 = 20;         // ATOM_COMBOPHY_PLL0 (clock_source_id_to_atom, COMBO_PHY_PLL0)
static constexpr uint8_t kEncUniphy = 0x1E;          // ENCODER_OBJECT_ID_INTERNAL_UNIPHY (encoder 0x211E's id)
static constexpr uint8_t kCrtc1 = 0;                 // ATOM_CRTC1 (controller_id_to_atom2, CONTROLLER_ID_D0)
// transmitter_control_v1_7: HPO_instance = (uint8_t)(cntl->hpo_engine_id - ENGINE_ID_HPO_0). The TMDS callers leave
// hpo_engine_id zeroed (ENGINE_ID_DIGA = 0) and ENGINE_ID_HPO_0 is 10 (grph_object_id.h), so Linux sends 0xF6.
static constexpr uint8_t kHpoInstanceTmds = (uint8_t)(0 - 10);

enum class Signal { Dvi, Hdmi };

// struct dmub_cmd_header: type 7:0, sub_type 15:8, ret_status 16, multi_cmd_pending 17, is_reg_based 18,
// payload_bytes 29:24 (all others 0, as the memset in the *_dmcub functions leaves them)
inline uint32_t header(uint8_t sub, uint8_t payload) { return (uint32_t)kCmdVbios | (uint32_t)sub << 8 | (uint32_t)payload << 24; }

static inline void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

// dmub_dig_transmitter_control_data_v1_7 at byte 4:
//   +0 phyid, +1 action, +2 digmode, +3 lanenum, +4 symclk_10khz (u32), +8 hpdsel, +9 digfe_sel, +10 connobj_id,
//   +11 HPO_instance, +12 reserved1, +13 skip_phy_ssc_reduction (link->wa_flags, 0), +14 reserved2[2], +16 reserved3[11]
// PHY on (dcn10_link_encoder_enable_tmds_output): lanes 4 (8 only for dual-link DVI), hpd_sel, pixel_clock = phy_pix_clk
// in kHz -> symclk_10khz = kHz / 10, connector_obj_id not set (0), digfe_sel = dig_encoder_sel_to_atom = 0.
inline void transmitterEnable(uint8_t out[kCmdBytes], Signal s, uint32_t phyPixKhz) {
    memset(out, 0, kCmdBytes);
    put32(out, header(kSubDig1TransmitterControl, kPayloadTransmitter));
    uint8_t *d = out + 4;
    d[0] = kPhyUniphyA;
    d[1] = kActionEnable;
    d[2] = s == Signal::Hdmi ? kDigModeHdmi : kDigModeDvi;
    d[3] = 4;
    put32(d + 4, phyPixKhz / 10);
    d[8] = kHpd1;
    d[9] = 0;
    d[10] = 0;
    d[11] = kHpoInstanceTmds;
}

// PHY off (dcn10_link_encoder_disable_output): transmitter, hpd_sel, signal, connector_obj_id; lanes and clock 0.
inline void transmitterDisable(uint8_t out[kCmdBytes], Signal s) {
    memset(out, 0, kCmdBytes);
    put32(out, header(kSubDig1TransmitterControl, kPayloadTransmitter));
    uint8_t *d = out + 4;
    d[0] = kPhyUniphyA;
    d[1] = kActionDisable;
    d[2] = s == Signal::Hdmi ? kDigModeHdmi : kDigModeDvi;
    d[8] = kHpd1;
    d[10] = kConnHdmiA;
    d[11] = kHpoInstanceTmds;
}

// set_pixel_clock_parameter_v1_7 at byte 4:
//   +0 pixclk_100hz (u32), +4 pll_id, +5 encoderobjid, +6 encoder_mode, +7 miscinfo, +8 crtc_id, +9 deep_color_ratio
// dcn31_program_pix_clk for HDMI/DVI at 8 bpc: target = actual_pix_clk_100hz (dcn3_get_pix_clk_dividers: no deep color
// scaling at 8 bpc), encoder_mode from encoder_mode_bp_to_atom2(signal, false), deep_color_ratio
// TRANSMITTER_COLOR_DEPTH_24 -> PIXEL_CLOCK_V7_DEEPCOLOR_RATIO_DIS (0); flags: SET_XTALIN_REF_SRC (use_external_clk is
// 0) -> PIXEL_CLOCK_V7_MISC_REF_DIV_SRC_XTALIN, which is 0x00: miscinfo 0.
inline void setPixelClock(uint8_t out[kCmdBytes], Signal s, uint32_t pixelKhz) {
    memset(out, 0, kCmdBytes);
    put32(out, header(kSubSetPixelClock, kPayloadPixelClock));
    uint8_t *d = out + 4;
    put32(d, pixelKhz * 10);
    d[4] = kComboPhyPll0;
    d[5] = kEncUniphy;
    d[6] = s == Signal::Hdmi ? kDigModeHdmi : kDigModeDvi;
    d[7] = 0;
    d[8] = kCrtc1;
    d[9] = 0;
}

// dig_encoder_stream_setup_parameters_v1_5 at byte 4:
//   +0 digid, +1 action, +2 digmode, +3 lanenum, +4 pclk_10khz (u32), +8 bitpercolor, +9 dplinkrate_270mhz
// enc314_stream_encoder_hdmi_set_stream_attribute / _dvi_: action SETUP, engine_id = the stream encoder (DIG FE 0 =
// ENGINE_ID_DIGA = 0), pixel_clock = actual_pix_clk_khz (HDMI) or pix_clk_100hz / 10 (DVI), lanes 4. The callers leave
// color_depth COLOR_DEPTH_UNDEFINED, so encoder_control_digx_v1_5's switch leaves bitpercolor 0 and pclk unscaled.
inline void encoderStreamSetup(uint8_t out[kCmdBytes], Signal s, uint32_t pixelKhz) {
    memset(out, 0, kCmdBytes);
    put32(out, header(kSubDigxEncoderControl, kPayloadEncoder));
    uint8_t *d = out + 4;
    d[0] = 0;
    d[1] = kEncoderCmdStreamSetup;
    d[2] = s == Signal::Hdmi ? kDigModeHdmi : kDigModeDvi;
    d[3] = 4;
    put32(d + 4, pixelKhz / 10);
}

} // namespace phxdmubcmd
