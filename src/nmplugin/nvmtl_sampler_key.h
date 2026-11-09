/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

#import <Foundation/Foundation.h>

// Keep the decimal key contract while avoiding boxing and formatting for the
// 16 sampler slots accepted by the binding paths. Preserve the old fallback
// for every other NSUInteger value, including invalid binding indices.
static inline NSString *nvmtl_sampler_index_key(NSUInteger index) {
    static NSString *const keys[16] = {
        @"0", @"1", @"2", @"3", @"4", @"5", @"6", @"7",
        @"8", @"9", @"10", @"11", @"12", @"13", @"14", @"15"
    };
    return index < 16 ? keys[index] : [@(index) stringValue];
}
