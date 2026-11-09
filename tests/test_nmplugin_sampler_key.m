/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#import "../src/nmplugin/nvmtl_sampler_key.h"

int main(void) {
    @autoreleasepool {
        NSMutableArray<NSNumber *> *indices = [NSMutableArray new];
        for (NSUInteger index = 0; index < 512; ++index)
            [indices addObject:@(index)];
        [indices addObjectsFromArray:@[
            @(UINT32_MAX), @(UINT64_C(1) << 32), @(NSUIntegerMax / 2),
            @(NSUIntegerMax - 1), @(NSUIntegerMax)
        ]];

        NSMutableDictionary *legacyMap = [NSMutableDictionary new];
        NSMutableDictionary *cachedMap = [NSMutableDictionary new];
        for (NSNumber *number in indices) {
            NSUInteger index = number.unsignedIntegerValue;
            NSString *key = nvmtl_sampler_index_key(index);
            // Render bindings used NSNumber.stringValue; compute used %lu.
            NSString *legacy = [@(index) stringValue];
            NSString *formatted = [NSString stringWithFormat:@"%lu", (unsigned long)index];
            assert([key isEqualToString:legacy]);
            assert([key isEqualToString:formatted]);
            if (index < 16)
                assert(key == nvmtl_sampler_index_key(index));
            legacyMap[legacy] = number;
            cachedMap[key] = number;
        }
        assert([legacyMap isEqualToDictionary:cachedMap]);

        // Exercise shared immutable dictionary lookup from concurrent callers.
        NSDictionary *lookup = [cachedMap copy];
        _Atomic unsigned mismatches = 0;
        _Atomic unsigned *failures = &mismatches;
        dispatch_apply(8, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^(size_t worker) {
            @autoreleasepool {
                for (NSUInteger i = 0; i < 32768; ++i) {
                    NSUInteger index = (i + worker) & 15;
                    NSString *key = nvmtl_sampler_index_key(index);
                    NSNumber *value = lookup[key];
                    if (!value || value.unsignedIntegerValue != index ||
                        key != nvmtl_sampler_index_key(index))
                        atomic_fetch_add(failures, 1);
                }
            }
        });
        assert(atomic_load(failures) == 0);

        // A binding inserted under one representation must be removable by the other.
        for (NSUInteger index = 0; index < 16; ++index) {
            NSString *legacy = [@(index) stringValue];
            NSString *key = nvmtl_sampler_index_key(index);
            assert([cachedMap[legacy] isEqual:legacyMap[key]]);
            [cachedMap removeObjectForKey:legacy];
            [legacyMap removeObjectForKey:key];
        }
        assert([legacyMap isEqualToDictionary:cachedMap]);
        printf("sampler key checks passed: %lu indices, %u concurrent lookups\n",
               (unsigned long)indices.count, 8u * 32768u);
    }
    return 0;
}
