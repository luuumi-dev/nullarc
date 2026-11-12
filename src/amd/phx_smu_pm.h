// phx_smu_pm.h: the GFX clock policy PhxBringup applies through the SMU 13.0.4 mailbox (debug.phx_pm_*), as pure
// functions so tests/test_phx_smu_pm.cpp can drive them without hardware.
//
//   range:    the floor/ceiling the owner asked for (0 = the PMFW's own DpmClocks_t MinGfxClk / MaxGfxClk), clamped
//             into the PMFW's range, floor <= ceiling (Linux smu_v13_0_4_set_soft_freq_limited_range refuses the rest)
//   governor: an ondemand step on SoftMaxGfxClk. Busy at or above `up` %: the ceiling at once. Below: the cap that would
//             hold the load at `target` %, rounded up to 100 MHz, falling at most `stepDown` MHz per tick so a short
//             lull does not drop the clock to the floor.
#pragma once

namespace phxpm {

struct Range {
    int lo, hi;
};

inline Range clampRange(int reqMin, int reqMax, int dpmMin, int dpmMax) {
    int lo = reqMin > 0 ? reqMin : dpmMin, hi = reqMax > 0 ? reqMax : dpmMax;
    lo = lo < dpmMin ? dpmMin : lo > dpmMax ? dpmMax : lo;
    hi = hi < lo ? lo : hi > dpmMax ? dpmMax : hi;
    return Range{lo, hi};
}

// cap: the SoftMax in force (0 = none yet, start from the ceiling); busy: GfxActivity in %
inline int governorStep(int cap, int busy, Range r, int up, int target, int stepDown) {
    if (cap <= 0 || cap > r.hi)
        cap = r.hi;
    if (cap < r.lo)
        cap = r.lo;
    if (busy < 0)
        busy = 0;
    int want;
    if (busy >= up) {
        want = r.hi;
    } else {
        if (target < 10)
            target = 10;
        want = (cap * busy / target + 99) / 100 * 100;
        const int floor = cap - (stepDown > 0 ? stepDown : 100);
        if (want < floor)
            want = floor;
    }
    return want < r.lo ? r.lo : want > r.hi ? r.hi : want;
}

}  // namespace phxpm
