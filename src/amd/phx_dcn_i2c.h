// phx_dcn_i2c.h: read a monitor's EDID over DDC1 with the DCN 3.1.4 hardware I2C engine (Phoenix 1 and Phoenix 2), the
// way Linux amdgpu does it. Header-only and freestanding: PhxBringup's debug selector (kernel) and tests/test_phx_dcn_i2c
// (host, a simulated engine) share it, and PhxAMDFB will.
//
// Sequence: drivers/gpu/drm/amd/display/dc/dce/dce_i2c_hw.c, as DCN 3.1.4 configures it:
//   dcn314_resource.c: i2c_inst_regs(1) for DDC line 0 (I2C_HW_ENGINE_COMMON_REG_LIST_DCN30), I2C_COMMON_MASK_SH_LIST_DCN30
//     (so no DC_I2C_DDC1_CLK_EN; START_STOP_TIMING_CNTL and SEND_RESET_LENGTH exist), enable_mem_low_power.bits.i2c = true,
//     caps.i2c_speed_in_khz = 100 (and _hdcp = 100), num_ddc = 5;
//   dcn2_i2c_hw_construct -> dcn1 -> dce112 -> dce100 -> dce_i2c_hw_construct: engine_id = the DDC line, setup_limit =
//     I2C_SETUP_TIME_LIMIT_DCN (3), send_reset_length = I2C_SEND_RESET_LENGTH_9, buffer_size = I2C_HW_BUFFER_SIZE_DCE100
//     (538), engine_keep_power_up_count = 1.
// The EDID transfer is drm_do_probe_ddc_edid's (drivers/gpu/drm/drm_edid.c): write the offset byte to 0x50, then read
// 128 bytes from 0x50, one I2C command of two payloads (amdgpu_dm_i2c_xfer -> dc_submit_i2c -> dce_i2c_submit_command_hw).
//
// Registers: dword offsets from drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_1_4_offset.h plus the DMU segment base
// (IP discovery DMU 3.1.4 bases 0x12 0xc0 0x34c0 on both chips); fields from dcn_3_1_4_sh_mask.h.
//
// The DDC pad is not reconfigured: GOP leaves DDC1 in hardware I2C mode (it read the EDID itself). Linux's dal_ddc_open
// (gpio/hw_ddc.c set_config, hw_gpio.c GPIO_MODE_HARDWARE) would set it; here a pad that is not in that mode (AUX mode or
// software GPIO mask set) refuses the read instead.
#pragma once
#include <stdint.h>

namespace phxi2c {

static constexpr uint32_t kSeg1 = 0xc0, kSeg2 = 0x34c0;   // DMU BASE_IDX 1 and 2
// BASE_IDX 2
static constexpr uint32_t kControl = kSeg2 + 0x1e98, kArbitration = kSeg2 + 0x1e99, kSwStatus = kSeg2 + 0x1e9b,
                          kDdc1HwStatus = kSeg2 + 0x1e9c, kDdc1Speed = kSeg2 + 0x1ea2, kDdc1Setup = kSeg2 + 0x1ea3,
                          kTransaction0 = kSeg2 + 0x1eae, kData = kSeg2 + 0x1eb2, kDioMemPwrStatus = kSeg2 + 0x1edd,
                          kDioMemPwrCtrl = kSeg2 + 0x1ede, kGpioDdc1Mask = kSeg2 + 0x28d0;
// BASE_IDX 1
static constexpr uint32_t kMicrosecondTimeBaseDiv = kSeg1 + 0x007b;
static constexpr uint32_t kHighestReg = kGpioDdc1Mask;

// DC_I2C_CONTROL
static constexpr uint32_t kCtlGo = 0x1, kCtlSoftReset = 0x2, kCtlSendReset = 0x4, kCtlSwStatusReset = 0x8,
                          kCtlDdcSelectMask = 0x700, kCtlDdcSelectShift = 8, kCtlTransCountMask = 0x300000,
                          kCtlTransCountShift = 20;
// DC_I2C_ARBITRATION
static constexpr uint32_t kArbRwCntlStatusMask = 0xc, kArbRwCntlStatusShift = 2, kArbNoQueuedSwGo = 0x10,
                          kArbSwUseReq = 0x100000, kArbSwDone = 0x200000;
// DC_I2C_SW_STATUS
static constexpr uint32_t kSwStatusMask = 0x3, kSwDone = 0x4, kSwAborted = 0x10, kSwTimeout = 0x20,
                          kSwStoppedOnNack = 0x100;
// DC_I2C_DDC1_HW_STATUS
static constexpr uint32_t kHwStatusMask = 0x3;
// DC_I2C_DDC1_SPEED
static constexpr uint32_t kSpdThresholdMask = 0x3, kSpdStartStopMask = 0x300, kSpdStartStopShift = 8,
                          kSpdPrescaleMask = 0xffff0000u, kSpdPrescaleShift = 16;
// DC_I2C_DDC1_SETUP
static constexpr uint32_t kSetDataDriveEn = 0x1, kSetDataDriveSel = 0x2, kSetSendResetLength = 0x4, kSetEnable = 0x40,
                          kSetClkDriveEn = 0x80, kSetIntraByteDelayMask = 0xff00, kSetIntraTransDelayMask = 0xff0000,
                          kSetTimeLimitMask = 0xff000000u, kSetTimeLimitShift = 24;
// DC_I2C_TRANSACTIONn (n at +n)
static constexpr uint32_t kTrRw = 0x1, kTrStopOnNack = 0x100, kTrStart = 0x1000, kTrStop = 0x2000,
                          kTrCountMask = 0x3ff0000, kTrCountShift = 16;
// DC_I2C_DATA
static constexpr uint32_t kDataRw = 0x1, kDataDataMask = 0xff00, kDataDataShift = 8, kDataIndexMask = 0x3ff0000,
                          kDataIndexShift = 16, kDataIndexWrite = 0x80000000u;
// MICROSECOND_TIME_BASE_DIV
static constexpr uint32_t kUsDivMask = 0x7f, kXtalRefDivMask = 0x7f00, kXtalRefDivShift = 8;
// DIO_MEM_PWR_CTRL / _STATUS
static constexpr uint32_t kI2cLightSleepForce = 0x1, kI2cMemPwrState = 0x1;
// DC_GPIO_DDC1_MASK
static constexpr uint32_t kGpioClkMask = 0x1, kGpioDataMask = 0x100, kGpioAuxPadMode = 0x10000;

// dce_i2c_hw.h
enum : uint32_t { kStatusIdle = 0, kStatusUsedBySw = 1, kStatusUsedByHw = 2, kRwCntlDmcuOnly = 2 };
static constexpr uint32_t kSpeedKhz = 100;        // dcn314 caps.i2c_speed_in_khz (and _hdcp)
static constexpr uint32_t kSetupLimit = 3;        // I2C_SETUP_TIME_LIMIT_DCN
static constexpr uint32_t kSendResetLength = 9;   // I2C_SEND_RESET_LENGTH_9 (a 1-bit field: REG_UPDATE writes 9 & 1)
static constexpr uint32_t kBufferSize = 538;      // I2C_HW_BUFFER_SIZE_DCE100
static constexpr uint32_t kTimeoutClocks = 32;    // TRANSACTION_TIMEOUT_IN_I2C_CLOCKS
static constexpr uint8_t kEdidAddr = 0x50;        // DDC_ADDR (drm_edid.c)

enum Result : uint32_t {
    kOk = 0,
    kPadNotI2c,          // DC_GPIO_DDC1_MASK: AUX mode or software GPIO mask (pad not left in hardware I2C mode)
    kEngineUnavailable,  // is_engine_available: used by HW, or DMCU-only arbitration
    kMemPowerTimeout,    // DIO_MEM_PWR_STATUS.I2C_MEM_PWR_STATE did not reach 0 (REG_WAIT, 5 tries)
    kNotAcquired,        // acquire_engine: arbitration not granted to SW
    kNoTimeBase,         // MICROSECOND_TIME_BASE_DIV is 0: the reference clock would need the VBIOS crystal
    kBusy,               // is_hw_busy before a transaction
    kNack,               // DC_I2C_SW_STOPPED_ON_NACK: no monitor answered at 0x50
    kTimeout,            // DC_I2C_SW_TIMEOUT, or still busy after Linux's transaction timeout
    kAborted,            // DC_I2C_SW_ABORTED
    kBadHeader,          // EDID block 0 header is not 00 FF FF FF FF FF FF 00
    kBadChecksum,        // an EDID block's bytes do not sum to 0
};

static inline const char *resultName(uint32_t r) {
    static const char *const k[] = { "ok", "DDC1 pad not in hardware I2C mode", "I2C engine used by HW/DMCU",
                                     "DIO I2C memory did not power up", "I2C arbitration not granted",
                                     "MICROSECOND_TIME_BASE_DIV is 0", "I2C engine busy", "no ACK at 0x50 (no monitor?)",
                                     "I2C timeout", "I2C aborted", "bad EDID header", "bad EDID checksum" };
    return r < sizeof(k) / sizeof(k[0]) ? k[r] : "?";
}

// Hw: uint32_t rd(uint32_t dword); void wr(uint32_t dword, uint32_t value); void delayUs(uint32_t us);
template <typename Hw> class Ddc1 {
    Hw &hw;
    uint32_t usedWrite = 0, usedBytes = 0, transCount = 0;   // dce_i2c_hw buffer_used_write / buffer_used_bytes / transaction_count
    uint8_t slave = kEdidAddr;   // the 7-bit address of the current command (EDID 0x50, SCDC 0x54)

    void update(uint32_t reg, uint32_t clear, uint32_t set) { hw.wr(reg, (hw.rd(reg) & ~clear) | set); }   // REG_UPDATE_N
    uint32_t swStatus() { return hw.rd(kSwStatus) & kSwStatusMask; }

    // is_engine_available
    bool engineAvailable() {
        if ((hw.rd(kDdc1HwStatus) & kHwStatusMask) == kStatusUsedByHw) return false;
        if (((hw.rd(kArbitration) & kArbRwCntlStatusMask) >> kArbRwCntlStatusShift) == kRwCntlDmcuOnly) return false;
        return true;
    }
    // is_hw_busy
    bool hwBusy() { return swStatus() != kStatusIdle && !engineAvailable(); }

    // set_speed: prescale from MICROSECOND_TIME_BASE_DIV (the VBIOS-crystal fallback is refused, kNoTimeBase)
    bool setSpeed(uint32_t speed) {
        const uint32_t v = hw.rd(kMicrosecondTimeBaseDiv);
        const uint32_t refBaseDiv = v & kUsDivMask;
        uint32_t xtalRefDiv = (v & kXtalRefDivMask) >> kXtalRefDivShift;
        if (!xtalRefDiv) xtalRefDiv = 2;
        if (!refBaseDiv) return false;
        const uint32_t prescale = (refBaseDiv * 1000 / xtalRefDiv) / speed;
        update(kDdc1Speed, kSpdPrescaleMask | kSpdThresholdMask | kSpdStartStopMask,
               ((prescale << kSpdPrescaleShift) & kSpdPrescaleMask) | 2u | ((speed > 50 ? 2u : 1u) << kSpdStartStopShift));
        return true;
    }

    // acquire_engine
    bool acquire() {
        uint32_t arb = (hw.rd(kArbitration) & kArbRwCntlStatusMask) >> kArbRwCntlStatusShift;
        if (arb == kStatusUsedBySw) return true;
        if (arb == kStatusUsedByHw) return false;
        update(kArbitration, 0, kArbSwUseReq);
        arb = (hw.rd(kArbitration) & kArbRwCntlStatusMask) >> kArbRwCntlStatusShift;
        return arb == kStatusUsedBySw;
    }

    // setup_engine (engine_id 0 = DDC line 0 = DDC1)
    uint32_t setup() {
        update(kControl, kCtlSoftReset, 0);
        update(kDioMemPwrCtrl, kI2cLightSleepForce, 0);   // enable_mem_low_power.bits.i2c
        bool on = false;
        for (int i = 0; i < 5 && !on; i++) {              // REG_WAIT(DIO_MEM_PWR_STATUS, I2C_MEM_PWR_STATE, 0, 0, 5)
            on = (hw.rd(kDioMemPwrStatus) & kI2cMemPwrState) == 0;
            if (!on) hw.delayUs(1);
        }
        if (!on) return kMemPowerTimeout;
        if (!acquire()) return kNotAcquired;
        if (!setSpeed(kSpeedKhz)) return kNoTimeBase;
        update(kControl, kCtlGo | kCtlSendReset | kCtlTransCountMask | kCtlDdcSelectMask, kCtlSwStatusReset);
        update(kDdc1Setup, kSetTimeLimitMask | kSetSendResetLength | kSetEnable,
               (kSetupLimit << kSetTimeLimitShift) | ((kSendResetLength << 2) & kSetSendResetLength) | kSetEnable);
        update(kArbitration, kArbNoQueuedSwGo, 0);
        return kOk;
    }

    // release_engine (with cntl_stuck_hw_workaround)
    void release() {
        if (swStatus() == kStatusUsedBySw) update(kControl, 0, kCtlSoftReset | kCtlSwStatusReset);
        else update(kControl, 0, kCtlSwStatusReset);
        // engine_keep_power_up_count = 1: DC_I2C_DDC1_ENABLE stays set
        setSpeed(kSpeedKhz);
        update(kArbitration, 0, kArbSwDone);
        if (((hw.rd(kArbitration) & kArbRwCntlStatusMask) >> kArbRwCntlStatusShift) == kStatusUsedBySw)
            update(kArbitration, 0, kArbSwDone);
        update(kDioMemPwrCtrl, 0, kI2cLightSleepForce);
    }

    // get_channel_status: 0 busy, 1 ok, else a Result
    uint32_t channelStatus() {
        const uint32_t v = hw.rd(kSwStatus);
        if ((v & kSwStatusMask) == kStatusUsedBySw) return 0;
        if (v & kSwStoppedOnNack) return kNack;
        if (v & kSwTimeout) return kTimeout;
        if (v & kSwAborted) return kAborted;
        return 1;
    }

    // get_transaction_timeout_hw, in microseconds
    uint32_t transactionTimeout(uint32_t length) {
        const uint32_t period = (1000 * kTimeoutClocks) / kSpeedKhz;
        return period * (1 + (length << 3) + 1 + (usedBytes << 3) + (transCount << 1));
    }

    // process_transaction for one payload; returns true when it was the last one (then the engine runs)
    bool transaction(bool read, bool mot, uint8_t addr, const uint8_t *data, uint32_t length) {
        const uint32_t action = read ? (mot ? 0x50u : 0x10u) : (mot ? 0x40u : 0x00u);   // dce_i2c_transaction_action
        const bool last = transCount == 3 || action == 0x00 || (action & 0x10);
        const uint32_t tr = kTransaction0 + transCount;
        update(tr, kTrStopOnNack | kTrStart | kTrRw | kTrCountMask | kTrStop,
               kTrStopOnNack | kTrStart | (read ? kTrRw : 0) | ((length << kTrCountShift) & kTrCountMask) |
                   (last ? kTrStop : 0));
        uint32_t value;
        if (transCount == 0) {   // REG_SET_4(DC_I2C_DATA, 0, DATA_RW 0, DATA addr, INDEX 0, INDEX_WRITE 1)
            value = ((uint32_t)addr << kDataDataShift) | kDataIndexWrite;
            hw.wr(kData, value);
            usedWrite = 0;
        } else {                 // REG_SET_2(DC_I2C_DATA, 0, DATA_RW 0, DATA addr)
            value = (uint32_t)addr << kDataDataShift;
            hw.wr(kData, value);
        }
        usedWrite++;
        if (!read) {
            for (uint32_t i = 0; i < length; i++) {   // REG_SET_2(DC_I2C_DATA, value, INDEX_WRITE 0, DATA byte)
                hw.wr(kData, (value & ~(kDataIndexWrite | kDataDataMask)) | ((uint32_t)data[i] << kDataDataShift));
                usedWrite++;
            }
        }
        transCount++;
        usedBytes += (read ? length : 0) + 1;   // Linux adds the remaining length: 0 after a write's loop
        return last;
    }

    // execute_transaction (after reset_hw_engine)
    void execute() {
        update(kControl, 0, kCtlSwStatusReset);   // reset_hw_engine
        update(kDdc1Setup, kSetDataDriveEn | kSetClkDriveEn | kSetDataDriveSel | kSetIntraTransDelayMask |
                               kSetIntraByteDelayMask, 0);
        update(kControl, kCtlSoftReset | kCtlSwStatusReset | kCtlSendReset | kCtlGo | kCtlTransCountMask,
               ((transCount - 1) << kCtlTransCountShift) & kCtlTransCountMask);
        update(kControl, 0, kCtlGo);
        transCount = 0;
        usedBytes = 0;
    }

    // dce_i2c_hw_engine_submit_payload
    uint32_t payload(bool read, bool mot, uint8_t *data, uint32_t length) {
        if (length >= kBufferSize - usedBytes) return kBusy;
        const uint8_t addr = (uint8_t)((slave << 1) | (read ? 1 : 0));
        const uint32_t timeout = transactionTimeout(length + 1);
        if (hwBusy()) return kBusy;   // process_transaction's check
        if (transaction(read, mot, addr, data, length)) {
            if (hwBusy()) return kBusy;   // submit_channel_request_hw's check
            execute();
        }
        uint32_t st = 1;   // dce_i2c_hw_engine_wait_on_operation_result: while busy, at most `timeout` us
        for (uint32_t us = 0; us < timeout; us += 10) {
            st = channelStatus();
            if (st != 0) break;
            hw.delayUs(10);
        }
        if (st == 0) return kTimeout;
        if (st != 1) return st;
        if (read) {   // process_channel_reply
            hw.wr(kData, ((usedWrite << kDataIndexShift) & kDataIndexMask) | kDataRw | kDataIndexWrite);
            for (uint32_t i = 0; i < length; i++) data[i] = (uint8_t)((hw.rd(kData) & kDataDataMask) >> kDataDataShift);
        }
        return kOk;
    }

public:
    explicit Ddc1(Hw &h) : hw(h) {}

    // Read-only preconditions: the pad in hardware I2C mode, the engine not held by HW or the DMCU.
    uint32_t check() {
        const uint32_t m = hw.rd(kGpioDdc1Mask);
        if (m & (kGpioAuxPadMode | kGpioClkMask | kGpioDataMask)) return kPadNotI2c;
        if (!engineAvailable()) return kEngineUnavailable;
        return kOk;
    }

    // One drm_do_probe_ddc_edid transfer: offset byte written to 0x50, `length` (<= 128) bytes read back.
    uint32_t read(uint8_t offset, uint8_t *out, uint32_t length) { return readAt(kEdidAddr, offset, out, length); }

    // The same two-payload command to any 7-bit address (link_query_ddc_data with a write and a read buffer:
    // dce_i2c_submit_command_hw sets MOT on every payload but the last)
    uint32_t readAt(uint8_t addr, uint8_t offset, uint8_t *out, uint32_t length) {
        slave = addr;
        uint32_t r = check();
        if (r) return r;
        usedWrite = usedBytes = transCount = 0;
        r = setup();
        if (r == kOk) {
            r = setSpeed(kSpeedKhz) ? kOk : kNoTimeBase;   // dce_i2c_submit_command_hw: set_speed(cmd->speed)
            if (r == kOk) r = payload(false, true, &offset, 1);
            if (r == kOk) r = payload(true, false, out, length);
        }
        if (r != kPadNotI2c && r != kEngineUnavailable) release();
        return r;
    }

    // One write payload of `length` bytes to a 7-bit address (link_query_ddc_data with a write buffer only)
    uint32_t write(uint8_t addr, const uint8_t *data, uint32_t length) {
        slave = addr;
        uint32_t r = check();
        if (r) return r;
        usedWrite = usedBytes = transCount = 0;
        r = setup();
        if (r == kOk) {
            r = setSpeed(kSpeedKhz) ? kOk : kNoTimeBase;
            uint8_t buf[16];
            if (length > sizeof buf) r = kBusy;
            for (uint32_t i = 0; r == kOk && i < length; i++) buf[i] = data[i];
            if (r == kOk) r = payload(false, false, buf, length);
        }
        if (r != kPadNotI2c && r != kEngineUnavailable) release();
        return r;
    }
};

static inline bool edidChecksumOk(const uint8_t *b) {
    uint8_t s = 0;
    for (int i = 0; i < 128; i++) s = (uint8_t)(s + b[i]);
    return s == 0;
}

// Block 0 and, when it announces one, block 1 (offset 128): what an HDMI monitor's CEA extension needs. Returns the
// bytes read (128 or 256) in *len.
template <typename Hw> uint32_t readEdid(Hw &hw, uint8_t out[256], uint32_t *len) {
    static const uint8_t kHeader[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    Ddc1<Hw> ddc(hw);
    *len = 0;
    uint32_t r = ddc.read(0, out, 128);
    if (r) return r;
    for (int i = 0; i < 8; i++)
        if (out[i] != kHeader[i]) return kBadHeader;
    if (!edidChecksumOk(out)) return kBadChecksum;
    *len = 128;
    if (out[126] == 0) return kOk;
    r = ddc.read(128, out + 128, 128);
    if (r) return r;
    if (!edidChecksumOk(out + 128)) return kBadChecksum;
    *len = 256;
    return kOk;
}

} // namespace phxi2c
