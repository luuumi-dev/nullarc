#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../accel/phx_panel_timing.h"

int main() {
    // Live read-only OTG0 snapshot, Radeon 780M 1002:1900, 2026-11-07.
    const phx::OtgTimingRegisters panel = {0x00000a9f, 0x00700a70, 0x00200000, 1,
        0x0000069d, 0x005b069b, 0x00060000, 1, 0x00011201, 0, 0};
    phx::PanelTiming t = {};
    unsigned cases = 0;
    assert(phx::decodePanelTiming(panel, panel, 2560, 1600, 120000, t)); ++cases;
    assert(t.width == 2560 && t.height == 1600 && t.hTotal == 2720 && t.vTotal == 1694);
    assert(t.hSyncOffset == 48 && t.hSyncWidth == 32 && t.vSyncOffset == 3 && t.vSyncWidth == 6);
    assert(!t.hPositive && !t.vPositive && t.pixelClock == 552921600);
    auto rejected = [&](phx::OtgTimingRegisters r, uint32_t w = 2560, uint32_t h = 1600, uint32_t rate = 120000) {
        t.pixelClock = UINT64_MAX;
        assert(!phx::decodePanelTiming(r, r, w, h, rate, t));
        assert(t.pixelClock == 0 && t.width == 0 && t.hTotal == 0); ++cases;
    };
    auto r = panel; r.control &= ~1u; rejected(r);
    r = panel; r.interlace = 1; rejected(r);
    for (uint32_t bit = 1; bit <= 0x20; bit <<= 1) {r = panel; r.vTotalControl = bit; rejected(r);}
    r = panel; r.hPolarity |= 0x10000; rejected(r);
    r = panel; r.vPolarity |= 0x100; rejected(r);
    rejected(panel, 1920); rejected(panel, 2560, 1080); rejected(panel, 0);
    rejected(panel, UINT32_MAX); rejected(panel, 2560, 1600, 23999);
    rejected(panel, 2560, 1600, 360001);
    r = panel; r.hTotal = 0; rejected(r);
    r = panel; r.hTotal = UINT32_MAX; rejected(r);
    r = panel; r.hBlank |= 0x80000000u; rejected(r);
    r = panel; r.vSync |= 0x8000u; rejected(r);
    r = panel; r.vTotal = 1599; rejected(r);
    r = panel; r.hBlank = 0x0a700a70; rejected(r);
    r = panel; r.vBlank = 0; rejected(r);
    r = panel; r.hSync = 0; rejected(r);
    r = panel; r.vSync = 0; rejected(r);
    r = panel; r.hSync = 0x0aa00000; rejected(r);
    r = panel; r.hSync = 0x00c80000; rejected(r);
    r = panel; r.vSync = 0x00640000; rejected(r);
    // Every snapshot field participates in the mode-change rejection.
    for (unsigned i = 0; i < 11; ++i) {
        r = panel;
        switch (i) {
        case 0: ++r.hTotal; break; case 1: ++r.hBlank; break; case 2: ++r.hSync; break;
        case 3: ++r.hPolarity; break; case 4: ++r.vTotal; break; case 5: ++r.vBlank; break;
        case 6: ++r.vSync; break; case 7: ++r.vPolarity; break; case 8: ++r.control; break;
        case 9: ++r.interlace; break; case 10: ++r.vTotalControl; break;
        }
        assert(!phx::decodePanelTiming(panel, r, 2560, 1600, 120000, t)); ++cases;
    }
    r = panel; r.hPolarity = r.vPolarity = 0;
    assert(phx::decodePanelTiming(r, r, 2560, 1600, 60000, t)); ++cases;
    assert(t.hPositive && t.vPositive && t.pixelClock == 276460800);
    // Fractional refresh is kept in mHz, with integer-Hz clock rounding.
    assert(phx::decodePanelTiming(panel, panel, 2560, 1600, 119880, t)); ++cases;
    assert(t.pixelClock == 552368678);
    // Largest supported geometry/rate must not overflow the intermediate clock product.
    r = {32767, (1u << 16) | 16385u, 1u << 16, 0,
         32767, (1u << 16) | 16385u, 1u << 16, 0, 1, 0, 0};
    assert(phx::decodePanelTiming(r, r, 16384, 16384, 360000, t)); ++cases;
    assert(t.pixelClock == 386547056640ULL);
    puts("panel timing regression tests passed");
    printf("validated cases: %u\n", cases);
}
