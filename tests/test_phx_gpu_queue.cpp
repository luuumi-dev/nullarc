#include "phxbringup/PhxGpuQueue.h"
#include <cassert>
#include <cstdint>

int main() {
    PhxGpuQueue q{};
    uint32_t slot = 99;
    assert(!phxQueueOldest(q, 0, slot));
    // Counter wrap preserves order, including the physical slot wrap.
    q.head = UINT32_MAX - 1;
    q.tail = 1;
    q.fseq[62] = 10;
    q.fseq[63] = 11;
    q.fseq[0] = 12;
    assert(phxQueueOldest(q, 10, slot) && slot == 63);
    assert(phxQueueOldest(q, 11, slot) && slot == 0);
    assert(!phxQueueOldest(q, 12, slot));
    // An inconsistent history must never trigger billions of iterations.
    q.head = 2;
    q.tail = 1;
    assert(!phxQueueOldest(q, 100, slot));

    q = {};
    q.tail = 1;
    q.fseq[0] = 1;
    q.fbinding[0] = 7;
    assert(phxQueueHangAction(q, 0, 2999, 3000, slot) == PhxHangAction::None);
    assert(phxQueueHangAction(q, 0, 3000, 3000, slot) == PhxHangAction::Vmid);
    assert(phxQueueKillValid(q, slot, 0, 1, 7));
    assert(!phxQueueKillValid(q, slot, 1, 1, 7)); // fence advanced during check
    assert(!phxQueueKillValid(q, slot, 0, 0, 7)); // owner freed
    assert(!phxQueueKillValid(q, slot, 0, 1, 8)); // same handle, new binding
    q.killedSeq = 1;
    q.lastProgress = 3000;
    assert(phxQueueHangAction(q, 0, 4499, 3000, slot) == PhxHangAction::None);
    assert(phxQueueHangAction(q, 0, 4500, 3000, slot) == PhxHangAction::All);
    q.killedAll = true;
    assert(phxQueueHangAction(q, 0, 9000, 3000, slot) == PhxHangAction::None);
    assert(phxQueueHangAction(q, 1, 9100, 3000, slot) == PhxHangAction::None);
    assert(q.lastDone == 1 && q.lastProgress == 9100);
    q.tail = 2;
    q.fseq[1] = 2;
    assert(phxQueueHangAction(q, 1, 12099, 3000, slot) == PhxHangAction::None);
    assert(phxQueueHangAction(q, 1, 12100, 3000, slot) == PhxHangAction::Vmid);
}
