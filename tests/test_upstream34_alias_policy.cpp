#include "../src/phxaccel/PhxHardwarePolicy.hpp"
#include <cassert>
#include <cstdio>

static void aperture_bounds_tests() {
    using phx780m::rangeWithin;
    static_assert(rangeWithin(0, 8, 8), "exact-fit access");
    static_assert(!rangeWithin(UINT64_MAX - 7, 8, 4096), "doorbell end must not wrap");
    static_assert(!rangeWithin(UINT64_MAX - 0x5fff, 0x6000, 0x1400000), "ring extent must not wrap");
    static_assert(!rangeWithin(UINT64_MAX, 1, UINT64_MAX), "end beyond addressable bound");
    assert(rangeWithin(0, 0, 0));
    assert(rangeWithin(4096, 0, 4096));
    assert(!rangeWithin(4097, 0, 4096));
    assert(rangeWithin(4088, 8, 4096));
    assert(!rangeWithin(4089, 8, 4096));
    assert(!rangeWithin(0, UINT64_MAX, 4096));
    assert(rangeWithin(UINT64_MAX - 1, 1, UINT64_MAX));
    // Exhaustively compare small ranges without invoking MMIO or loading a driver.
    for (uint64_t limit = 0; limit <= 32; ++limit)
        for (uint64_t offset = 0; offset <= 40; ++offset)
            for (uint64_t bytes = 0; bytes <= 40; ++bytes)
                assert(rangeWithin(offset, bytes, limit) == (offset <= limit && offset + bytes <= limit));
}
static void pool_release_tests() {
    using phx780m::poolReleaseValid;
    static_assert(poolReleaseValid(0, 4096, 4096), "one-page pool release");
    static_assert(!poolReleaseValid(0, 0, 4096), "zero-length release");
    static_assert(!poolReleaseValid(0, 4097, 8192), "not page-granular");
    static_assert(!poolReleaseValid(1, 4096, 8192), "unaligned offset");
    static_assert(!poolReleaseValid(UINT64_MAX - 4095, 4096, 8192), "offset must not wrap into the pool");
    static_assert(!poolReleaseValid(4096, 4096, 4096), "extent must not exceed the pool");
    assert(poolReleaseValid(0, 8192, 8192));
    assert(poolReleaseValid(8192, 0x1000, 0x100000));
    assert(!poolReleaseValid(8192, 0x1001, 0x100000));
    assert(!poolReleaseValid(0, 0x1000, 0));
    assert(!poolReleaseValid(0x1000, 0x1000, 0x1000));
    // Exhaustive small pool: valid exactly for nonzero page multiples that fit.
    for (uint64_t poolBytes = 0; poolBytes <= 4 * 4096; poolBytes += 4096)
        for (uint64_t offset = 0; offset <= 5 * 4096; offset += 4096)
            for (uint64_t pages = 0; pages <= 6; ++pages) {
                const uint64_t bytes = pages * 4096;
                assert(poolReleaseValid(offset, bytes, poolBytes) ==
                       (bytes != 0 && offset + bytes <= poolBytes));
            }
}
static void alias_policy_tests() {
    using phx780m::aliasCarveOutReady;
    using phx780m::aliasExhausted;
    using phx780m::aliasSamplesMatch;
    using phx780m::aliasSampleStep;
    // A present made before the carve-out is published is a wait, not a failed check: counting them as failures
    // pinned a session to the ~110 ms/frame BAR copy path for its whole life.
    static_assert(!aliasExhausted(0, 0), "a fresh session may try");
    static_assert(!aliasExhausted(0, 3599), "3599 waits spend no verification check");
    static_assert(!aliasExhausted(2, 3599), "two mismatches and many waits may still try once more");
    static_assert(aliasExhausted(3, 0), "three pixel mismatches stop the alias for the session");
    static_assert(aliasExhausted(0, 3600), "the wait budget is bounded as well");
    assert(phx780m::kAliasVerifyLimit == 3 && phx780m::kAliasWaitLimit == 3600);
    // Both facts are needed, and their sum must not wrap.
    static_assert(!aliasCarveOutReady(0, 0x1000), "no published carve-out address");
    static_assert(!aliasCarveOutReady(0x1000, ~0ull), "scan-out not inside any BAR range");
    static_assert(aliasCarveOutReady(0x1000, 0), "published, and inside the BAR at offset 0");
    static_assert(!aliasCarveOutReady(~0ull, 1), "the addition would wrap");
    for (uint64_t carve = 0; carve < 8; ++carve)
        for (uint64_t bar = 0; bar < 8; ++bar)
            assert(aliasCarveOutReady(carve, bar) == (carve != 0 && bar != ~0ull && carve <= ~0ull - bar));
    // About 4096 samples across the surface, at least one word apart.
    static_assert(aliasSampleStep(0) == 1, "an empty surface still steps by one word");
    static_assert(aliasSampleStep(4095) == 1, "smaller than the sample budget");
    static_assert(aliasSampleStep(4096) == 1, "exactly the sample budget");
    static_assert(aliasSampleStep(2560ull * 1600) == 1000, "2560x1600 words: one sample per 1000 words");
    assert(aliasSampleStep(1ull << 40) == (1ull << 28));
    // Every sample must match, and a view of zeros is not the same pixels.
    static_assert(!aliasSamplesMatch(0, 0, 0), "no samples is not a match");
    static_assert(!aliasSamplesMatch(100, 100, 0), "all matching but all zero: a stale view");
    static_assert(!aliasSamplesMatch(99, 100, 100), "one mismatch refuses the alias");
    static_assert(aliasSamplesMatch(100, 100, 1), "all matching with at least one non-zero word");
    assert(aliasSamplesMatch(4096, 4096, 4096));
    assert(!aliasSamplesMatch(4096, 4096, 0));
}
int main() { aperture_bounds_tests(); pool_release_tests(); alias_policy_tests(); puts("pool/alias policies passed (CPU only)"); }
