// phx_dmub_inbox.h: send 64-byte DMUB commands to the driver's DMCUB firmware through inbox 1, as Linux v6.17's
// dmub_srv does on DCN 3.1 (dmub/src/dmub_srv.c, dmub/inc/dmub_cmd.h, dmub/src/dmub_dcn31.c). Pure sequence over
// callbacks: register read/write and VRAM dword read/write (tools/phxctl/testl4.cpp drives PhxBringup's debug
// selectors; tests/test_phx_dmub_inbox.cpp a fake).
//
//   ready:    dmub_dcn31_is_hw_init: DMCUB_CNTL.DMCUB_ENABLE and dmub_fw_boot_status.dal_fw (DMCUB_SCRATCH0 bit 0);
//             mailbox_rdy (bit 1) as dmub_srv_wait_for_auto_load waits for it. GOP's firmware has dal_fw 0: Linux
//             (and Test J's dmcubload) replaces it before sending display commands.
//   locate:   the ring is inbox 1: DMCUB sees it at DMCUB_INBOX1_BASE_ADDRESS (inside the CW4 window,
//             dmub_dcn31_setup_mailbox), the GPU address behind CW4 is DMCUB_REGION3_CW4_OFFSET(_HIGH)
//             (dmub_dcn31_setup_windows REG_WRITEs both halves whole). VRAM offset = that MC address - the frame
//             buffer's MC base + (inbox base - CW4 base). Capacity = DMCUB_INBOX1_SIZE.
//   queue:    dmub_srv_fb_cmd_queue / dmub_rb_push_front: refuse when a pointer exceeds the capacity or the ring is
//             full (one slot always unused), copy the command at wrpt, wrpt += 64 modulo the capacity
//   execute:  dmub_srv_fb_cmd_execute: read the queued commands back (dmub_rb_flush_pending), then INBOX1_WPTR = wrpt
//   wait:     dmub_srv_wait_for_idle: DMCUB_INBOX1_RPTR until it equals wrpt (1 us steps; dm_execute_dmub_cmd waits
//             up to 100000 us), HW failure when RPTR exceeds the capacity (dmub_srv_update_inbox_status)
#pragma once
#include "phx_dcn314_prog_regs.h"
#include <stdint.h>

namespace phxdmubinbox {

using phxdcn314::kCw4Base;
using phxdcn314::kCw4Offset;
using phxdcn314::kCw4OffsetHigh;
using phxdcn314::kDmcubEnable;
using phxdcn314::kDmcubScratch0;
using phxdcn314::kInbox1Base;
using phxdcn314::kInbox1Rptr;
using phxdcn314::kInbox1Size;
using phxdcn314::kInbox1Wptr;

static constexpr uint32_t kCmdBytes = 64;   // DMUB_RB_CMD_SIZE
static constexpr uint32_t kBootDalFw = 0x1, kBootMailboxRdy = 0x2;   // union dmub_fw_boot_status

struct Io {
    uint32_t (*rd)(void *ctx, uint32_t reg);
    void (*wr)(void *ctx, uint32_t reg, uint32_t val);
    uint32_t (*vramRd)(void *ctx, uint64_t off);
    void (*vramWr)(void *ctx, uint64_t off, uint32_t val);
    void (*delayUs)(void *ctx, unsigned us);
    void *ctx;
};

enum class Status { Ok, NotReady, BadWindow, HwFailure, QueueFull, FlushMismatch, Timeout };
inline const char *name(Status s) {
    switch (s) {
    case Status::Ok: return "OK";
    case Status::NotReady: return "DMCUB not running the driver's firmware (dal_fw/mailbox_rdy/enable)";
    case Status::BadWindow: return "inbox 1 is not inside the CW4 window / VRAM";
    case Status::HwFailure: return "HW failure (a ring pointer past the capacity)";
    case Status::QueueFull: return "ring full";
    case Status::FlushMismatch: return "command read back differently from VRAM";
    case Status::Timeout: return "DMCUB did not consume the command in time";
    }
    return "?";
}

struct Inbox {
    uint64_t vramOff = 0;   // VRAM byte offset of the ring
    uint32_t base = 0, capacity = 0, cw4Base = 0;
    uint64_t cw4Mc = 0;
    uint32_t wrpt = 0, rptr = 0;
    uint32_t lastWptr = 0;  // dmub->inbox1_last_wptr
};

inline bool ready(const Io &io, uint32_t *scratch0 = nullptr) {
    const uint32_t s0 = io.rd(io.ctx, kDmcubScratch0.addr);
    if (scratch0) *scratch0 = s0;
    const bool en = (io.rd(io.ctx, kDmcubEnable.addr) & kDmcubEnable.mask) != 0;
    return en && (s0 & (kBootDalFw | kBootMailboxRdy)) == (kBootDalFw | kBootMailboxRdy);
}

// Where inbox 1 is, from the registers alone. vramSize bounds the result (the BAR the debug selectors reach).
inline Status locate(const Io &io, uint64_t fbMcBase, uint64_t vramSize, Inbox *ib) {
    ib->base = io.rd(io.ctx, kInbox1Base.addr);
    ib->capacity = io.rd(io.ctx, kInbox1Size.addr);
    ib->cw4Base = io.rd(io.ctx, kCw4Base.addr);
    ib->cw4Mc = (uint64_t)io.rd(io.ctx, kCw4OffsetHigh.addr) << 32 | io.rd(io.ctx, kCw4Offset.addr);
    ib->wrpt = io.rd(io.ctx, kInbox1Wptr.addr);
    ib->rptr = io.rd(io.ctx, kInbox1Rptr.addr);
    ib->lastWptr = ib->wrpt;
    // DMCUB_REGION3_CW4_BASE_ADDRESS keeps 29 bits (dcn_3_1_4_sh_mask.h mask 0x1fffffff): written 0x64000000, it
    // reads back 0x04000000 (Test L step 4a, 2026-11-05). The inbox's offset into the window is taken in that width.
    const uint32_t inWin = ib->base & kCw4Base.mask, winBase = ib->cw4Base & kCw4Base.mask;
    if (!ib->capacity || ib->capacity % kCmdBytes || inWin < winBase || ib->cw4Mc < fbMcBase) return Status::BadWindow;
    ib->vramOff = ib->cw4Mc - fbMcBase + (inWin - winBase);
    if (ib->vramOff + ib->capacity > vramSize) return Status::BadWindow;
    if (ib->wrpt > ib->capacity || ib->rptr > ib->capacity) return Status::HwFailure;
    return Status::Ok;
}

inline uint32_t outstanding(const Inbox &ib) {   // dmub_rb_num_outstanding, in bytes
    return ib.wrpt >= ib.rptr ? ib.wrpt - ib.rptr : ib.capacity - (ib.rptr - ib.wrpt);
}

inline Status queue(const Io &io, Inbox &ib, const uint8_t cmd[kCmdBytes]) {
    ib.rptr = io.rd(io.ctx, kInbox1Rptr.addr);   // dmub_srv_update_inbox_status, before Linux queues
    if (ib.rptr > ib.capacity || ib.wrpt > ib.capacity) return Status::HwFailure;
    if (outstanding(ib) == ib.capacity - kCmdBytes) return Status::QueueFull;
    for (uint32_t i = 0; i < kCmdBytes; i += 4) {
        const uint32_t v = (uint32_t)cmd[i] | (uint32_t)cmd[i + 1] << 8 | (uint32_t)cmd[i + 2] << 16 | (uint32_t)cmd[i + 3] << 24;
        io.vramWr(io.ctx, ib.vramOff + ib.wrpt + i, v);
    }
    ib.wrpt += kCmdBytes;
    if (ib.wrpt >= ib.capacity) ib.wrpt %= ib.capacity;
    return Status::Ok;
}

// The read-back of the queued commands (dmub_rb_flush_pending) is also compared with what was written.
inline Status execute(const Io &io, Inbox &ib, const uint8_t *cmds, unsigned n) {
    uint32_t p = ib.lastWptr;
    for (unsigned c = 0; c < n && p != ib.wrpt; c++) {
        for (uint32_t i = 0; i < kCmdBytes; i += 4) {
            const uint8_t *b = cmds + c * kCmdBytes + i;
            const uint32_t want = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
            if (io.vramRd(io.ctx, ib.vramOff + p + i) != want) return Status::FlushMismatch;
        }
        p += kCmdBytes;
        if (p >= ib.capacity) p %= ib.capacity;
    }
    io.wr(io.ctx, kInbox1Wptr.addr, ib.wrpt);
    ib.lastWptr = ib.wrpt;
    return Status::Ok;
}

inline Status waitIdle(const Io &io, Inbox &ib, uint32_t timeoutUs = 100000) {
    for (uint32_t t = 0; t < timeoutUs; t++) {
        ib.rptr = io.rd(io.ctx, kInbox1Rptr.addr);
        if (ib.rptr > ib.capacity) return Status::HwFailure;
        if (ib.rptr == ib.wrpt) return Status::Ok;
        io.delayUs(io.ctx, 1);
    }
    return Status::Timeout;
}

// One command, the way dc_wake_and_execute_dmub_cmd(..., DM_DMUB_WAIT_TYPE_WAIT) sends it: queue, execute, wait.
inline Status send(const Io &io, Inbox &ib, const uint8_t cmd[kCmdBytes], uint32_t timeoutUs = 100000) {
    Status s = queue(io, ib, cmd);
    if (s != Status::Ok) return s;
    s = execute(io, ib, cmd, 1);
    if (s != Status::Ok) return s;
    return waitIdle(io, ib, timeoutUs);
}

}  // namespace phxdmubinbox
