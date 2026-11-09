// phx_dcn314_clk.h: Test L step 3. Raise DISPCLK and DPPCLK to what Linux's DML gives for 144 Hz, and set the live
// pipe's DPP DTO, as Linux's DCN 3.1.4 clock manager does when it goes up (dcn314_update_clocks, safe_to_lower
// false); then come back down the way it lowers. Pure sequence over phx_dcn314_step2.h's Io callbacks.
//
// Linux v6.17, clk_mgr/dcn314/dcn314_clk_mgr.c dcn314_update_clocks, for the live HDMI stream:
//   dcn314_disable_otg_wa: touches only streams that are dpms_off or virtual: nothing here
//   should_set_clock(dispclk) -> dcn314_smu_set_dispclk: VBIOSSMC_MSG_SetDispclkFreq (0x4), khz_to_mhz_ceil
//   not lowered: dcn314_smu_set_dppclk: VBIOSSMC_MSG_SetDppclkFreq (0x6), then dcn20_update_clocks_update_dpp_dto:
//       ref_dppclk = the requested global DPPCLK, dccg31_update_dpp_dto: modulo 0xff,
//       phase = ceil(modulo * pipe dppclk / ref_dppclk), DPPCLK0_DTO_PARAM, then DPPCLK_DTO_CTRL.DPPCLK0_DTO_ENABLE 1
//   lowered: the DTO first, then SetDppclkFreq ("increase per DPP DTO before lowering global dppclk")
// Left out, each because it changes nothing on this pipe:
//   - SetHardMinDcfclkByFreq 200 MHz and SetMinDeepSleepDcfclk: DML's DCFCLK is DPM level 0 (Test clk: 200 MHz),
//     so it is already the floor
//   - z-states, DTBCLK (HDMI uses none) and display-idle optimizations (dcn314 leaves debug.pstate_enabled unset)
//   - DMUB_CMD__CLK_MGR_NOTIFY_CLOCKS: DMCUB uses these clocks only for PSR / idle features, which an HDMI link
//     doesn't run; there is no host-to-DMCUB command path yet
// The SMU mailbox is dcn314_smu.c's: VBIOSSMC message MP1_SMN_C2PMSG_67, argument _83, response _91 (MP1 segment 0
// 0x16000, mp_13_0_5_offset.h), dcn314_smu_wait_for_response polling _91 every 10 us up to 200000 times.
#pragma once
#include "phx_dcn314_step2.h"
#include <stdint.h>

namespace phxdcn314 {

static constexpr uint32_t kSmuMsg = 0x16000 + 0x283, kSmuArg = 0x16000 + 0x293, kSmuResp = 0x16000 + 0x29b;
static constexpr uint32_t kSmuBusy = 0x0, kSmuOk = 0x1, kSmuFailed = 0xFF;
static constexpr uint32_t kMsgSetDispclkFreq = 0x4, kMsgSetDppclkFreq = 0x6;   // dcn314_smu.c VBIOSSMC_MSG_*

inline unsigned khzToMhzCeil(unsigned khz) { return (khz + 999) / 1000; }   // clk_mgr_internal.h

// dccg31_update_dpp_dto: phase / modulo = pipe DPPCLK / global DPPCLK
inline unsigned dppDtoPhase(unsigned refKhz, unsigned reqKhz) {
    const unsigned modulo = 0xff;
    unsigned phase = (unsigned)(((uint64_t)modulo * reqKhz + refKhz - 1) / refKhz);
    return phase > 0xff ? 0xff : phase;
}

// dentist_get_divider_from_did (dce_clk_mgr.c), x4 fixed point (DENTIST_DIVIDER_RANGE_SCALE_FACTOR)
inline unsigned dentistDivider(unsigned did) {
    if (did < 0x08) did = 0x08;
    if (did > 0x7f) did = 0x7f;
    if (did < 0x40) return 8 + (did - 0x08);
    if (did < 0x60) return 64 + 2 * (did - 0x40);
    if (did < 0x7e) return 128 + 4 * (did - 0x60);
    return 248 + 264 * (did - 0x7e);
}
inline unsigned dentistKhz(unsigned vcoKhz, unsigned did) { return (unsigned)((uint64_t)vcoKhz * 4 / dentistDivider(did)); }

// dcn314_smu_send_msg_with_param: wait for the previous message, clear the response, argument, message, wait.
// Returns the response; *out = the argument register after (the clock the SMU set, in MHz).
inline uint32_t smuWait(const Io &io) {
    uint32_t r = kSmuBusy;
    for (unsigned i = 0; i <= 200000; i++) {
        r = io.rd(io.ctx, kSmuResp);
        if (r != kSmuBusy) break;
        io.delayUs(io.ctx, 10);
    }
    return r;
}
inline uint32_t smuSend(const Io &io, uint32_t msg, uint32_t param, uint32_t *out) {
    uint32_t r = smuWait(io);
    if (r == kSmuBusy) return r;
    io.wr(io.ctx, kSmuResp, kSmuBusy);
    io.wr(io.ctx, kSmuArg, param);
    io.wr(io.ctx, kSmuMsg, msg);
    r = smuWait(io);
    if (r == kSmuFailed) io.wr(io.ctx, kSmuResp, kSmuOk);   // as Linux does after VBIOSSMC_Result_Failed
    *out = io.rd(io.ctx, kSmuArg);
    return r;
}

struct ClkStep {
    uint32_t dispResp = 0, dispMhz = 0, dppResp = 0, dppMhz = 0;
    bool ok() const { return dispResp == kSmuOk && dppResp == kSmuOk; }
};

// Going up (safe_to_lower false): SetDispclkFreq, SetDppclkFreq, then the pipe's DTO against the requested DPPCLK.
inline ClkStep raise(const Io &io, unsigned dispKhz, unsigned dppKhz, unsigned pipeDppKhz) {
    ClkStep s;
    s.dispResp = smuSend(io, kMsgSetDispclkFreq, khzToMhzCeil(dispKhz), &s.dispMhz);
    if (s.dispResp != kSmuOk) return s;
    s.dppResp = smuSend(io, kMsgSetDppclkFreq, khzToMhzCeil(dppKhz), &s.dppMhz);
    if (s.dppResp != kSmuOk) return s;
    const uint32_t param = fieldSet(fieldSet(0, kDppDtoPhase0, dppDtoPhase(dppKhz, pipeDppKhz)), kDppDtoModulo0, 0xff);
    io.wr(io.ctx, kDppDtoPhase0.addr, param);
    io.wr(io.ctx, kDppDtoEnable0.addr, fieldSet(io.rd(io.ctx, kDppDtoEnable0.addr), kDppDtoEnable0, 1));
    return s;
}

// Coming down (safe_to_lower, DPPCLK lowered): DISPCLK, then the DTO registers put back as saved, then DPPCLK.
inline ClkStep lower(const Io &io, unsigned dispKhz, unsigned dppKhz, uint32_t savedDtoParam, uint32_t savedDtoCtrl) {
    ClkStep s;
    s.dispResp = smuSend(io, kMsgSetDispclkFreq, khzToMhzCeil(dispKhz), &s.dispMhz);
    io.wr(io.ctx, kDppDtoPhase0.addr, savedDtoParam);
    io.wr(io.ctx, kDppDtoEnable0.addr, savedDtoCtrl);
    s.dppResp = smuSend(io, kMsgSetDppclkFreq, khzToMhzCeil(dppKhz), &s.dppMhz);
    return s;
}

}  // namespace phxdcn314
