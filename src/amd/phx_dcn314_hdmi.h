// phx_dcn314_hdmi.h: Test L step 5d. What an HDMI stream adds to the DVI one on DCN 3.1.4's DIG FE 0 (Linux v6.17):
//   enc314_stream_encoder_hdmi_set_stream_attribute (dio/dcn314): the DMUB DIGX_ENCODER_CONTROL STREAM_SETUP (HDMI,
//     actual_pix_clk_khz, no audio), the pixel-encoding helper, HDMI_CONTROL (PACKET_GEN_VERSION 1, KEEPOUT_MODE 1,
//     DEEP_COLOR_ENABLE 0, DATA_SCRAMBLE_EN 0, NO_EXTRA_NULL_PACKET_FILLED 1, CLOCK_CHANNEL_RATE 0; DEEP_COLOR_DEPTH 0 at
//     8 bpc; scrambling only at >= 340 MHz), the General Control packet every frame (GC_CONT, GC_SEND, NULL_SEND), no
//     ACP, HDMI_AUDIO_INFO_SEND 1 + afmt3_audio_info_immediate_update + HDMI_AUDIO_INFO_LINE = VBI_LINE_0 + 2, AVMUTE 0
//   enc3_stream_encoder_update_hdmi_info_packets (dio/dcn30): HDMI_DB_DISABLE 1, AFMT_AUDIO_CLOCK_EN 1, then the packets
//     in Linux's order (0 AVI, 5 HF-VSIF, 2 gamut, 1 vendor, 3 SPD, 4 HDR metadata, 6 VTEM): a valid packet goes into
//     the VPG (vpg3_update_generic_info_packet) and sends every frame on line 2, an invalid one gets CONT / SEND / LINE 0.
//     For the 8500G's 1080p RGB stream only the AVI InfoFrame is valid (phx_hdmi_avi.h): no 3D or HDMI VIC (vendor), no
//     HDMI 2.1 features (HF-VSIF / VTEM), no FreeSync (SPD), no HDR (metadata), no gamut packet.
//   vpg3_update_generic_info_packet (dcn30_vpg.c): wait VPG_GENERIC_CONFLICT_OCCURED 0 (10 us x 50), CONFLICT_CLR 1,
//     DATA_INDEX = index * 9, header dword (hb0..hb3), the 8 dwords of sb[0..31], GENERIC<n>_IMMEDIATE_UPDATE 1
//   enc3_stream_encoder_stop_hdmi_info_packets: HDMI_GENERIC_PACKET_CONTROL0-4 and 6-10 all zero
//   enc1_reset_hdmi_stream_attribute: HDMI_CONTROL PACKET_GEN_VERSION 1, KEEPOUT_MODE 1, DEEP_COLOR / SCRAMBLE / CLOCK
//     CHANNEL RATE 0;  enc1_stream_encoder_set_avmute: HDMI_GC.HDMI_GC_AVMUTE
//   write_scdc_data / read_scdc_data (link/protocols/link_ddc.c) over DDC1 at HDMI_SCDC_ADDRESS 0x54: read
//     SINK_VERSION (0x01); if it is 1, SOURCE_VERSION (0x02) = 1; TMDS_CONFIG (0x20) = 3 above 340 MHz, else 0 (no
//     LTE_340MCSC_SCRAMBLE). read_scdc_data reads TMDS_CONFIG back (status only).
// No audio endpoint: the macOS side has no HDMI audio, which is Linux's configuration without one (stream_res.audio
// NULL, as with amdgpu.audio=0): no az_configure / setup_audio_output, enable_audio false.
#pragma once
#include "phx_dcn314_step2.h"
#include "phx_hdmi_avi.h"

namespace phxdcn314 {

// DDC1 for SCDC: one link_query_ddc_data read (write offset, read one byte) and one write; false on an I2C failure
struct ScdcIo {
    bool (*read)(void *ctx, uint8_t offset, uint8_t *val);
    bool (*write)(void *ctx, const uint8_t *data, unsigned len);
    void *ctx;
};
static constexpr uint8_t kScdcSinkVersion = 0x01, kScdcSourceVersion = 0x02, kScdcTmdsConfig = 0x20;

struct ScdcResult { bool readOk = false, writeOk = false; uint8_t sinkVersion = 0, tmdsConfig = 0xff; };

// write_scdc_data
inline ScdcResult writeScdc(const ScdcIo &s, uint32_t pixKhz, bool lte340Scramble) {
    ScdcResult r;
    uint8_t v = 0;
    r.readOk = s.read(s.ctx, kScdcSinkVersion, &v);   // link_query_ddc_data leaves sink_version 0 when the read fails
    r.sinkVersion = r.readOk ? v : 0;
    bool ok = true;
    if (r.sinkVersion == 1) {
        const uint8_t b[2] = {kScdcSourceVersion, 1};
        ok = s.write(s.ctx, b, 2) && ok;
    }
    const uint8_t cfg = pixKhz > 340000 ? 3 : lte340Scramble ? 1 : 0;
    const uint8_t b[2] = {kScdcTmdsConfig, cfg};
    ok = s.write(s.ctx, b, 2) && ok;
    r.writeOk = ok;
    return r;
}
inline uint8_t readScdcTmdsConfig(const ScdcIo &s) {   // read_scdc_data's first read
    uint8_t v = 0xff;
    return s.read(s.ctx, kScdcTmdsConfig, &v) ? v : 0xff;
}

inline void hdmiUpd(const Io &io, const Fld &f, uint32_t v) { io.wr(io.ctx, f.addr, fieldSet(io.rd(io.ctx, f.addr), f, v)); }

inline void setAvmute(const Io &io, bool on) { hdmiUpd(io, kHdmiGcAvmute, on ? 1 : 0); }

inline void stopHdmiInfoPackets(const Io &io) {   // enc3_stream_encoder_stop_hdmi_info_packets
    const uint32_t regs[] = {kRegHdmiGenPktCtl0, kRegHdmiGenPktCtl1, kRegHdmiGenPktCtl0, kRegHdmiGenPktCtl2,
                             kRegHdmiGenPktCtl0, kRegHdmiGenPktCtl3, kRegHdmiGenPktCtl0, kRegHdmiGenPktCtl4,
                             kRegHdmiGenPktCtl6, kRegHdmiGenPktCtl7, kRegHdmiGenPktCtl6, kRegHdmiGenPktCtl8,
                             kRegHdmiGenPktCtl6, kRegHdmiGenPktCtl9, kRegHdmiGenPktCtl6, kRegHdmiGenPktCtl10};
    for (uint32_t r : regs) io.wr(io.ctx, r, 0);
}

inline void resetHdmiStreamAttribute(const Io &io) {   // enc1_reset_hdmi_stream_attribute
    uint32_t c = io.rd(io.ctx, kHdmiPktGenVer.addr);
    c = fieldSet(c, kHdmiPktGenVer, 1);
    c = fieldSet(c, kHdmiKeepout, 1);
    c = fieldSet(c, kHdmiDeepColorEn, 0);
    c = fieldSet(c, kHdmiScramble, 0);
    c = fieldSet(c, kHdmiClkChanRate, 0);
    io.wr(io.ctx, kHdmiPktGenVer.addr, c);
}

// the register part of enc314_stream_encoder_hdmi_set_stream_attribute after the DMUB command and the helper
inline void hdmiStreamAttributeRegs(const Io &io, uint32_t pixKhz) {
    uint32_t c = io.rd(io.ctx, kHdmiPktGenVer.addr);
    c = fieldSet(c, kHdmiPktGenVer, 1);
    c = fieldSet(c, kHdmiKeepout, 1);
    c = fieldSet(c, kHdmiDeepColorEn, 0);
    c = fieldSet(c, kHdmiScramble, 0);
    c = fieldSet(c, kHdmiNoExtraNull, 1);
    c = fieldSet(c, kHdmiClkChanRate, 0);
    io.wr(io.ctx, kHdmiPktGenVer.addr, c);
    hdmiUpd(io, kHdmiDeepColorDepth, 0);                 // COLOR_DEPTH_888
    if (pixKhz >= 340000) {                             // HDMI_CLOCK_CHANNEL_RATE_MORE_340M
        uint32_t d = io.rd(io.ctx, kHdmiScramble.addr);
        io.wr(io.ctx, kHdmiScramble.addr, fieldSet(fieldSet(d, kHdmiScramble, 1), kHdmiClkChanRate, 1));
    }
    uint32_t v = io.rd(io.ctx, kHdmiGcCont.addr);
    v = fieldSet(fieldSet(fieldSet(v, kHdmiGcCont, 1), kHdmiGcSend, 1), kHdmiNullSend, 1);
    io.wr(io.ctx, kHdmiGcCont.addr, v);
    hdmiUpd(io, kHdmiAcpSend, 0);
    hdmiUpd(io, kHdmiAudioInfoSend, 1);
    hdmiUpd(io, kAfmtAudioInfoUpdate, 1);               // afmt3_audio_info_immediate_update
    hdmiUpd(io, kHdmiAudioInfoLine, 0 + 2);             // VBI_LINE_0 + 2
    hdmiUpd(io, kHdmiGcAvmute, 0);
}

// vpg3_update_generic_info_packet with immediate_update, for packet 0 (the only valid one here)
inline bool vpgWritePacket0(const Io &io, const phxavi::InfoPacket &p) {
    bool idle = false;
    for (unsigned i = 0; i < 50 && !idle; i++) {   // REG_WAIT(VPG_GENERIC_STATUS, CONFLICT_OCCURED, 0, 10, 50)
        idle = fieldGet(io.rd(io.ctx, kVpgConflict.addr), kVpgConflict) == 0;
        if (!idle) io.delayUs(io.ctx, 10);
    }
    hdmiUpd(io, kVpgConflictClr, 1);
    hdmiUpd(io, kVpgDataIndex, 0 * 9);
    io.wr(io.ctx, kVpgData.addr, (uint32_t)p.hb0 | (uint32_t)p.hb1 << 8 | (uint32_t)p.hb2 << 16 | (uint32_t)p.hb3 << 24);
    for (unsigned i = 0; i < 8; i++)
        io.wr(io.ctx, kVpgData.addr, (uint32_t)p.sb[4 * i] | (uint32_t)p.sb[4 * i + 1] << 8 | (uint32_t)p.sb[4 * i + 2] << 16 |
                                     (uint32_t)p.sb[4 * i + 3] << 24);
    hdmiUpd(io, kVpgGen0Immediate, 1);
    return idle;
}

// enc3_update_hdmi_info_packet's control bits for one packet index (0..6)
inline void hdmiPacketControl(const Io &io, unsigned idx, bool valid) {
    static const Fld *const cont[7] = {&kHdmiGen0Cont, &kHdmiGen1Cont, &kHdmiGen2Cont, &kHdmiGen3Cont, &kHdmiGen4Cont, &kHdmiGen5Cont, &kHdmiGen6Cont};
    static const Fld *const send[7] = {&kHdmiGen0Send, &kHdmiGen1Send, &kHdmiGen2Send, &kHdmiGen3Send, &kHdmiGen4Send, &kHdmiGen5Send, &kHdmiGen6Send};
    static const Fld *const line[7] = {&kHdmiGen0Line, &kHdmiGen1Line, &kHdmiGen2Line, &kHdmiGen3Line, &kHdmiGen4Line, &kHdmiGen5Line, &kHdmiGen6Line};
    const uint32_t v = valid ? 1 : 0;
    io.wr(io.ctx, cont[idx]->addr, fieldSet(fieldSet(io.rd(io.ctx, cont[idx]->addr), *cont[idx], v), *send[idx], v));
    hdmiUpd(io, *line[idx], valid ? 2 : 0);
}

// enc3_stream_encoder_update_hdmi_info_packets with only the AVI InfoFrame valid
inline bool updateHdmiInfoPackets(const Io &io, const phxavi::InfoPacket &avi) {
    hdmiUpd(io, kHdmiDbDisable, 1);
    hdmiUpd(io, kAfmtAudioClockEn, 1);
    const bool idle = vpgWritePacket0(io, avi);
    hdmiPacketControl(io, 0, true);
    const unsigned rest[] = {5, 2, 1, 3, 4, 6};
    for (unsigned i : rest) hdmiPacketControl(io, i, false);
    return idle;
}

}  // namespace phxdcn314
