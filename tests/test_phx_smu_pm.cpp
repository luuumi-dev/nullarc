// tests/test_phx_smu_pm.cpp: the GFX clock policy of src/amd/phx_smu_pm.h (PhxBringup debug.phx_pm_*).
#include "phx_smu_pm.h"
#include <cstdio>
#include <cstdlib>

static int fails = 0;
#define CHECK(c, msg)                                                   \
    do {                                                                \
        if (c) std::printf("[PASS] %s\n", msg);                         \
        else { std::printf("[FAIL] %s (%s:%d)\n", msg, __FILE__, __LINE__); fails++; } \
    } while (0)

int main() {
    using namespace phxpm;
    // Phoenix PMFW range seen on the 780M: 400..2700 MHz (assumed here; the kext reads DpmClocks_t)
    Range d = clampRange(0, 0, 400, 2700);
    CHECK(d.lo == 400 && d.hi == 2700, "0/0 asks for the PMFW range");
    Range n = clampRange(800, 2000, 400, 2700);
    CHECK(n.lo == 800 && n.hi == 2000, "a narrower range is kept");
    Range c = clampRange(100, 9000, 400, 2700);
    CHECK(c.lo == 400 && c.hi == 2700, "out-of-range requests are clamped to the PMFW range");
    Range x = clampRange(2500, 1000, 400, 2700);
    CHECK(x.lo == 2500 && x.hi == 2500, "a ceiling under the floor is raised to it");

    CHECK(governorStep(0, 0, d, 70, 50, 300) == 2400, "first idle tick starts from the ceiling and steps down");
    CHECK(governorStep(2700, 95, d, 70, 50, 300) == 2700, "busy at/above up stays at the ceiling");
    CHECK(governorStep(400, 80, d, 70, 50, 300) == 2700, "a burst jumps from the floor straight to the ceiling");
    int cap = 2700, ticks = 0;
    while (cap > 400 && ticks < 100) { cap = governorStep(cap, 0, d, 70, 50, 300); ticks++; }
    CHECK(cap == 400 && ticks == 8, "idle reaches the floor in 8 ticks of 300 MHz (2 s at 250 ms)");
    CHECK(governorStep(2000, 40, d, 70, 50, 300) == 1700, "40% at 2000 sizes the cap for 50%: 1600 -> limited to 1700");
    CHECK(governorStep(1000, 60, d, 70, 50, 300) == 1200, "60% at 1000 raises the cap to 1200");
    CHECK(governorStep(1200, 50, d, 70, 50, 300) == 1200, "a load at the target holds the cap");
    CHECK(governorStep(500, 0, n, 70, 50, 300) == 800, "the cap never goes under the floor asked for");
    CHECK(governorStep(2700, 90, n, 70, 50, 300) == 2000, "nor over the ceiling asked for");
    CHECK(governorStep(1500, 20, d, 70, 0, 0) >= 400, "nonsense tunables stay in range");
    CHECK(governorStep(1500, -5, d, 70, 50, 300) == 1200, "negative busy counts as idle");
    if (fails) { std::printf("%d phx_smu_pm checks FAILED\n", fails); return EXIT_FAILURE; }
    std::printf("=== All phx_smu_pm tests PASSED ===\n");
    return 0;
}
