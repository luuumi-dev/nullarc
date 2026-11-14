#ifndef PHX_GPU_QUEUE_H
#define PHX_GPU_QUEUE_H

#include <stdint.h>

// All queue metadata and helper calls are protected by PhxBringup::gpuLock.
struct PhxGpuQueue {
    bool ready, gfx;
    uint32_t ringOff, ringDw, wbOff, fenceOff;
    uint64_t dbByte, wptr, doneEnd;
    uint64_t fseq[64], fend[64];
    uint32_t fvmid[64];
    uint64_t fbinding[64];                 // VMID binding generation at submission
    uint32_t head, tail;
    uint64_t lastDone, lastProgress;
    uint64_t killedSeq;
    bool killedAll;
};

inline bool phxQueueOldest(const PhxGpuQueue &q, uint64_t done, uint32_t &slot) {
    const uint32_t count = q.tail - q.head;
    for (uint32_t n = 0; n < count && n < 64; n++) {
        const uint32_t i = (q.head + n) & 63;
        if (q.fseq[i] > done) {
            slot = i;
            return true;
        }
    }
    return false;
}

enum class PhxHangAction { None, Vmid, All };

inline PhxHangAction phxQueueHangAction(PhxGpuQueue &q, uint64_t done, uint64_t now, uint64_t limit,
                                       uint32_t &slot) {
    if (!phxQueueOldest(q, done, slot) || done != q.lastDone) {
        q.lastDone = done;
        q.lastProgress = now;
        return PhxHangAction::None;
    }
    if (q.killedSeq != q.fseq[slot])
        return now - q.lastProgress >= limit ? PhxHangAction::Vmid : PhxHangAction::None;
    return !q.killedAll && now - q.lastProgress >= limit / 2 ? PhxHangAction::All : PhxHangAction::None;
}

inline bool phxQueueKillValid(const PhxGpuQueue &q, uint32_t slot, uint64_t done,
                             uint32_t owner, uint64_t binding) {
    return q.fseq[slot] > done && owner != 0 && q.fbinding[slot] == binding;
}

#endif
