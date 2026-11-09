// phx_hdmi_avi.h: the AVI InfoFrame Linux sends on an HDMI stream, built as Linux v6.17 builds it
// (dc/core/dc_resource.c set_avi_info_frame, into union hdmi_info_packet from include/set_mode_types.h). Pure
// computation; tests/test_phx_hdmi_avi.cpp compares it byte for byte with Linux's own packed union, copied verbatim.
//
// For the RGB, 8 bpc, sRGB, progressive, borderless stream the 8500G drives, with what amdgpu_dm v6.17 puts in the stream:
//   timing.vic: drm_hdmi_avi_infoframe_from_display_mode's video_code (amdgpu_dm.c fill_stream_properties_from_drm_display
//               _mode): the CTA-861 VIC the mode matches (1920x1080@60 = 16; the 144 Hz mode matches none: 0)
//   timing.hdmi_vic 0, timing_3d_format NONE, timing.rid / fr_index 0 (amdgpu_dm sets neither)
//   timing.aspect_ratio = mode_in->picture_aspect_ratio (get_aspect_ratio): NONE for a mode from an EDID detailed
//               timing, and for any mode of a client without DRM_CLIENT_CAP_ASPECT_RATIO -> M0_M1 0
//   content_type: the connector's "content type" property, default No Data -> CN0_CN1 0, ITC 1
//   qs_bit: never set in v6.17 -> Q0_Q1 RGB_QUANTIZATION_DEFAULT_RANGE (0)
// Fields: Y 0 (RGB), A0 ACTIVE_FORMAT_VALID 1, B0_B1 BAR_INFO_BOTH_VALID 3, SC0_SC1 PICTURE_SCALING_UNIFORM 0,
// S0_S1 SCANNING_TYPE_UNDERSCAN 2, C0_C1 COLORIMETRY_NO_DATA 0 (sRGB), R0_R3 ACTIVE_FORMAT_ASPECT_RATIO_SAME_AS_PICTURE 8,
// YQ0_YQ1 YYC_QUANTIZATION_LIMITED_RANGE 0, bar_top = v_border_top 0, bar_bottom = v_total - v_border_bottom + 1,
// bar_left 0, bar_right = h_total - h_border_right + 1 (sic), header type 0x82, version 2 (VIC < 128), length 13
// (HDMI_AVI_INFOFRAME_SIZE); checksum over sb[1..length] plus the three header values, one byte complement.
#pragma once
#include <stdint.h>
#include <string.h>

namespace phxavi {

struct InfoPacket { bool valid; uint8_t hb0, hb1, hb2, hb3; uint8_t sb[32]; };   // struct dc_info_packet

static constexpr uint8_t kTypeAvi = 0x82, kAviLength = 13;

// aspect: enum dc_aspect_ratio (0 no data, 1 4:3, 2 16:9; anything else -> 0)
inline InfoPacket avi(unsigned vic, unsigned aspect, unsigned hTotal, unsigned vTotal) {
    InfoPacket p;
    memset(&p, 0, sizeof p);
    uint8_t sb[28] = {0};
    const uint8_t version = vic >= 128 ? 3 : 2;
    const unsigned y = 0, a0 = 1, b = 3, s = 2, c = 0, m = (aspect == 1 || aspect == 2) ? aspect : 0, r = 8;
    const unsigned itc = 1, ec = 0, q = 0, sc = 0, pr = 0, cn = 0, yq = 0;
    sb[1] = (uint8_t)(s | b << 2 | a0 << 4 | y << 5);                 // S0_S1:2 B0_B1:2 A0:1 Y0_Y1_Y2:3
    sb[2] = (uint8_t)(r | m << 4 | c << 6);                           // R0_R3:4 M0_M1:2 C0_C1:2
    sb[3] = (uint8_t)(sc | q << 2 | ec << 4 | itc << 7);              // SC0_SC1:2 Q0_Q1:2 EC0_EC2:3 ITC:1
    sb[4] = (uint8_t)vic;                                            // VIC0_VIC7
    sb[5] = (uint8_t)(pr | cn << 4 | yq << 6);                        // PR0_PR3:4 CN0_CN1:2 YQ0_YQ1:2
    const unsigned barTop = 0, barBottom = vTotal + 1, barLeft = 0, barRight = hTotal + 1;
    sb[6] = (uint8_t)barTop;     sb[7] = (uint8_t)(barTop >> 8);
    sb[8] = (uint8_t)barBottom;  sb[9] = (uint8_t)(barBottom >> 8);
    sb[10] = (uint8_t)barLeft;   sb[11] = (uint8_t)(barLeft >> 8);
    sb[12] = (uint8_t)barRight;  sb[13] = (uint8_t)(barRight >> 8);
    uint8_t sum = (uint8_t)(kTypeAvi + kAviLength + version);
    for (unsigned i = 1; i <= kAviLength; i++) sum = (uint8_t)(sum + sb[i]);
    sb[0] = (uint8_t)(0x100 - sum);
    p.valid = true;
    p.hb0 = kTypeAvi; p.hb1 = version; p.hb2 = kAviLength;
    memcpy(p.sb, sb, 28);
    return p;
}

}  // namespace phxavi
