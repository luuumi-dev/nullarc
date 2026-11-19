#pragma once

#import <Foundation/Foundation.h>
#include <stdint.h>

static inline id nvmtl_surface_key(uint32_t surfaceID, NSUInteger plane) {
    if (plane <= 0xffu)
        return @(((uint64_t)surfaceID << 8) | (uint64_t)plane);
    return [NSString stringWithFormat:@"%u:%lu", surfaceID, (unsigned long)plane];
}
