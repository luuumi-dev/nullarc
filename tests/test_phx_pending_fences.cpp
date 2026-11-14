#include "phxbringup/PhxPendingFences.h"
#include <cassert>
#include <cstring>

int main() {
    PhxPendingFences q;
    uint64_t oldBuffer = 0, reusedHandleBuffer = 0, otherBuffer = 0;
    PhxPendingFence a{10, reinterpret_cast<uint8_t *>(&oldBuffer), nullptr, nullptr};
    PhxPendingFence b{11, reinterpret_cast<uint8_t *>(&otherBuffer), nullptr, nullptr};
    PhxPendingFence c{12, reinterpret_cast<uint8_t *>(&oldBuffer), nullptr, nullptr};
    assert(!q.take(100));
    q.append(&a);
    q.append(&b);
    q.append(&c);
    assert(!q.take(9));
    auto publish = [&](uint64_t retired) {
        unsigned count = 0;
        while (auto *f = q.take(retired)) {
            std::memcpy(f->destination, &f->seq, sizeof(f->seq));
            count++;
        }
        return count;
    };
    assert(publish(11) == 2);
    assert(oldBuffer == 10 && otherBuffer == 11 && reusedHandleBuffer == 0);
    assert(q.head == &c && q.tail == &c);
    assert(publish(12) == 1 && oldBuffer == 12);
    assert(!q.head && !q.tail);
    q.append(&b);
    assert(q.take(11) == &b && !q.head && !q.tail);
}
