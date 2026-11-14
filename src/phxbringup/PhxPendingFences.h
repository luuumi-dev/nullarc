#ifndef PHX_PENDING_FENCES_H
#define PHX_PENDING_FENCES_H
#include <stdint.h>
class IOMemoryDescriptor;

// Caller serializes access and owns each node and its retained descriptor.
struct PhxPendingFence {
    uint64_t seq;
    uint8_t *destination;
    IOMemoryDescriptor *memory;
    PhxPendingFence *next;
};
struct PhxPendingFences {
    PhxPendingFence *head{nullptr}, *tail{nullptr};
    void append(PhxPendingFence *f) {
        f->next = nullptr;
        if (tail)
            tail->next = f;
        else
            head = f;
        tail = f;
    }
    PhxPendingFence *take(uint64_t retired) {
        PhxPendingFence *f = head;
        if (!f || f->seq > retired)
            return nullptr;
        head = f->next;
        if (!head)
            tail = nullptr;
        return f;
    }
};
#endif
