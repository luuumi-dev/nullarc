// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Framebuffer pool and scanout-alias validation; no hardware access.
#ifndef PHX_780M_HARDWARE_POLICY_HPP
#define PHX_780M_HARDWARE_POLICY_HPP
#include <stdint.h>
namespace phx780m {
constexpr bool rangeWithin(uint64_t offset, uint64_t bytes, uint64_t limit) {
    return offset <= limit && bytes <= limit - offset;
}
constexpr bool pageAligned(uint64_t bytes) {
    return bytes != 0 && (bytes & 0xFFFull) == 0;
}
// VRAM pool release: the extent must be page-granular and fit inside the pool, so a bad or
// foreign handle cannot hand the range allocator a partial or wrapped range to free. The
// caller converts pointers to integers first and checks its base before subtracting.
constexpr bool poolReleaseValid(uint64_t offset, uint64_t bytes, uint64_t poolBytes) {
    return pageAligned(bytes) && (offset & 0xFFFull) == 0 && rangeWithin(offset, bytes, poolBytes);
}
// The scan-out alias (docs/DISPLAY-PATH.md, docs/analysis.md §1.2): the carve-out view is used only once
// PhxBringup publishes where it sits, and only if it holds the pixels the BAR shows. A present made before the
// publication is a wait, not a failed check -- counting three early presents as verification failures pinned a
// session to the ~110 ms/frame BAR copy path for its whole life.
constexpr unsigned kAliasVerifyLimit = 3;    // pixel checks a mismatch may spend
constexpr unsigned kAliasWaitLimit = 3600;   // presents made before the carve-out is published
constexpr bool aliasExhausted(unsigned checks, unsigned waits) {
    return checks >= kAliasVerifyLimit || waits >= kAliasWaitLimit;
}
// Both facts are needed before a view can be mapped and verified: the published carve-out address, and the
// offset of this scan-out inside the BAR. The addition must not wrap.
constexpr bool aliasCarveOutReady(uint64_t carveOutPhys, uint64_t barOffset) {
    return carveOutPhys != 0 && barOffset != ~static_cast<uint64_t>(0) &&
           carveOutPhys <= ~static_cast<uint64_t>(0) - barOffset;
}
// Roughly 4096 samples spread over the surface, at least one word apart.
constexpr uint64_t aliasSampleStep(uint64_t words) {
    return words / 4096 ? words / 4096 : 1;
}
// Accepted only when every sample matches the BAR view and at least one sample is non-zero: a zero-filled view
// matches a stale BAR, which is not the same pixels.
constexpr bool aliasSamplesMatch(uint64_t same, uint64_t samples, uint64_t nonZero) {
    return samples != 0 && same == samples && nonZero != 0;
}
}
#endif
