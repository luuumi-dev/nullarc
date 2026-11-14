#ifndef PHX_RANGE_H
#define PHX_RANGE_H
#include <stdint.h>

inline bool phxRangeFits(uint64_t offset, uint64_t size, uint64_t limit) {
    return offset <= limit && size <= limit - offset;
}
#endif
