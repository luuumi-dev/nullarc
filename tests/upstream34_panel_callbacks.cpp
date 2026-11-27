// CPU doubles for actual refresh measurement and timing callback; no real MMIO.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "accel/phx_panel_timing.h"
using IOReturn = int;
using IODisplayModeID = uint32_t;
constexpr int kIOReturnSuccess = 0, kIOReturnUnsupportedMode = -3;
// The native SDK build separately verifies actual Apple structures and constants.
constexpr uint32_t kIOTimingIDInvalid = 0xffffffffu, kIODetailedTimingValid = 1;
constexpr uint32_t kIODigitalSignal = 1, kIOSyncPositivePolarity = 1;
constexpr unsigned kIOPCIConfigBaseAddress5 = 0x24;
struct TimingV2 {
    uint64_t pixelClock, minPixelClock, maxPixelClock;
    uint32_t horizontalActive, horizontalBlanking, horizontalSyncOffset, horizontalSyncPulseWidth;
    uint32_t verticalActive, verticalBlanking, verticalSyncOffset, verticalSyncPulseWidth;
    uint32_t signalConfig, horizontalSyncConfig, verticalSyncConfig;
};
struct IOTimingInformation {
    uint32_t appleTimingID, flags;
    struct { TimingV2 v2; } detailedInfo;
    uint32_t reserved[4];
};
#define PFBLOG(...) ((void)0)
#define OSSafeReleaseNULL(object) do { if (object) (object)->release(); (object) = nullptr; } while (0)
struct IOMemoryMap {
    std::vector<uint32_t> regs = std::vector<uint32_t>(0x5100);
    uint64_t length = 0x5100 * 4;
    unsigned releases = 0;
    uint64_t getLength() { return length; }
    uintptr_t getVirtualAddress() { return reinterpret_cast<uintptr_t>(regs.data()); }
    void release() { ++releases; }
};
struct MockPCI {
    IOMemoryMap *mapping = nullptr;
    IOMemoryMap *mapDeviceMemoryWithRegister(unsigned value) { assert(value == kIOPCIConfigBaseAddress5); return mapping; }
};
static constexpr unsigned counter = 0x34c0 + 0x1b4c;
static IOMemoryMap *activeMap;
static uint64_t clockNow;
static uint32_t rateMilliHz, fixedFrameAdvance;
static int counterPhase;
static uint64_t extraSleepNs;
static unsigned sleeps, sleptMilliseconds;
static bool changeMode, zeroElapsed;
static uint64_t mach_absolute_time() { return clockNow; }
static void absolutetime_to_nanoseconds(uint64_t value, uint64_t *out) { *out = value; }
static void IOSleep(unsigned milliseconds) {
    assert(milliseconds == 50 || milliseconds == 200 || milliseconds == 2000); ++sleeps;
    sleptMilliseconds += milliseconds;
    const uint64_t elapsed = uint64_t(milliseconds) * 1000000 + extraSleepNs;
    if (!zeroElapsed) clockNow += elapsed;
    const uint32_t frames = fixedFrameAdvance != UINT32_MAX ? fixedFrameAdvance
        : uint32_t(int64_t(elapsed * rateMilliHz / 1000000000000ULL) + counterPhase);
    activeMap->regs[counter] = (activeMap->regs[counter] + frames) & 0xffffff;
    if (changeMode) ++activeMap->regs[0x34c0 + 0x1b2a];
}
struct Fixture {
    IOMemoryMap memory; MockPCI pci; MockPCI *fPCI = &pci;
    uint32_t fWidth = 2560, fHeight = 1600, fRefreshMilliHz = 60000, fHTotal = 0, fVTotal = 0;
    uint64_t fFrameNs = 16666667;
    phx::PanelTiming fPanelTiming = {};
    bool fPanelTimingValid = false;
    Fixture() {
        // Reviewed upstream fixture; these are not new target observations.
        const unsigned offsets[] = {0x1b2a,0x1b2b,0x1b2c,0x1b2d,0x1b2f,0x1b36,0x1b37,0x1b38,0x1b41,0x1b44,0x1b33};
        const uint32_t values[] = {0xa9f,0x00700a70,0x00200000,1,0x69d,0x005b069b,0x00060000,1,0x11201,0,0};
        for (unsigned i = 0; i < 11; ++i) memory.regs[0x34c0 + offsets[i]] = values[i];
        pci.mapping = &memory; activeMap = &memory; clockNow = 1000000;
        rateMilliHz = 120000; fixedFrameAdvance = UINT32_MAX; counterPhase = 0;
        extraSleepNs = 0; sleeps = sleptMilliseconds = 0; changeMode = zeroElapsed = false;
    }
    // ACTUAL_REFRESH_METHOD
    // ACTUAL_TIMING_METHOD
};
static unsigned checks = 0;
static void checked(bool value, int line) { if (!value) std::fprintf(stderr, "failed check at line %d\n", line); assert(value); ++checks; }
#define check(value) checked((value), __LINE__)
static void fallback(Fixture &f) {
    IOTimingInformation info; std::memset(&info, 0xa5, sizeof info);
    check(!f.fPanelTimingValid);
    check(f.getTimingInfoForDisplayMode(1, &info) == kIOReturnSuccess);
    IOTimingInformation expected{}; expected.appleTimingID = kIOTimingIDInvalid;
    check(std::memcmp(&info, &expected, sizeof info) == 0);
}
int main() {
    // Replay the logged 25-frame / 200,965-us input with a one-frame endpoint
    // phase. At 2 s the corresponding synthetic input is 241 / 2,000,965 us.
    // The assumed 120-Hz clock here is a CPU model, not a panel-rate finding.
    Fixture replay; extraSleepNs = 965000; counterPhase = 1; replay.measureRefresh();
    std::fprintf(stderr, "short-window regression replay: %u mHz\n", replay.fRefreshMilliHz);
    check(replay.fRefreshMilliHz == 120000 && replay.fPanelTimingValid);

    Fixture good; auto expectedRegisters = good.memory.regs;
    expectedRegisters[counter] = 240;
    good.measureRefresh();
    check(good.fPanelTimingValid && good.fRefreshMilliHz == 120000 && good.fFrameNs == 8333333);
    check(good.fHTotal == 2720 && good.fVTotal == 1694 && sleeps == 1 && sleptMilliseconds == 2000 && good.memory.releases == 1);
    check(good.memory.regs == expectedRegisters); // Only the CPU fake's frame counter update is permitted.
    IOTimingInformation info; std::memset(&info, 0xa5, sizeof info);
    check(good.getTimingInfoForDisplayMode(1, &info) == kIOReturnSuccess);
    check(info.appleTimingID == kIOTimingIDInvalid && info.flags == kIODetailedTimingValid);
    const auto &t = info.detailedInfo.v2;
    check(t.pixelClock == 552921600 && !t.minPixelClock && !t.maxPixelClock);
    check(t.horizontalActive == 2560 && t.horizontalBlanking == 160 && t.horizontalSyncOffset == 48 && t.horizontalSyncPulseWidth == 32);
    check(t.verticalActive == 1600 && t.verticalBlanking == 94 && t.verticalSyncOffset == 3 && t.verticalSyncPulseWidth == 6);
    check(t.signalConfig == kIODigitalSignal && !t.horizontalSyncConfig && !t.verticalSyncConfig);
    for (auto value : info.reserved) check(value == 0);
    auto before = info;
    check(good.getTimingInfoForDisplayMode(0, &info) == kIOReturnUnsupportedMode);
    check(std::memcmp(&info, &before, sizeof info) == 0);
    check(good.getTimingInfoForDisplayMode(2, &info) == kIOReturnUnsupportedMode);
    check(good.getTimingInfoForDisplayMode(1, nullptr) == kIOReturnUnsupportedMode);
    good.fPanelTiming.hPositive = good.fPanelTiming.vPositive = true;
    check(good.getTimingInfoForDisplayMode(1, &info) == kIOReturnSuccess);
    check(info.detailedInfo.v2.horizontalSyncConfig == kIOSyncPositivePolarity && info.detailedInfo.v2.verticalSyncConfig == kIOSyncPositivePolarity);
    good.fPanelTimingValid = false; fallback(good);

    Fixture wrap; wrap.memory.regs[counter] = 0xfffff8; wrap.measureRefresh();
    check(wrap.fPanelTimingValid && wrap.fRefreshMilliHz == 120000);
    Fixture missing; missing.fPCI = nullptr; missing.measureRefresh(); fallback(missing); check(sleeps == 0);
    Fixture noMap; noMap.pci.mapping = nullptr; noMap.measureRefresh(); fallback(noMap); check(sleeps == 0);
    Fixture shortMap; shortMap.memory.length -= 4; shortMap.measureRefresh(); fallback(shortMap);
    check(shortMap.memory.releases == 1 && sleeps == 0);
    Fixture changing; changeMode = true; changing.measureRefresh(); fallback(changing);
    Fixture inactive; inactive.memory.regs[0x34c0 + 0x1b41] = 0; inactive.measureRefresh(); fallback(inactive);
    Fixture interlaced; interlaced.memory.regs[0x34c0 + 0x1b44] = 1; interlaced.measureRefresh(); fallback(interlaced);
    Fixture variable; variable.memory.regs[0x34c0 + 0x1b33] = 1; variable.measureRefresh(); fallback(variable);
    Fixture wrongArea; wrongArea.fWidth = 1920; wrongArea.measureRefresh(); fallback(wrongArea);
    Fixture slow; fixedFrameAdvance = 3; slow.measureRefresh(); fallback(slow);
    Fixture highRate; rateMilliHz = 500000; highRate.measureRefresh(); fallback(highRate);
    Fixture noTime; zeroElapsed = true; noTime.measureRefresh(); fallback(noTime);

    const uint32_t standardRates[] = {48000,50000,60000,75000,90000,100000,120000,144000,165000,240000};
    for (uint32_t rate : standardRates) {
        for (int phase : {-1, 0, 1}) {
            for (uint64_t extra : {0ULL, 965000ULL, 1000000000ULL}) {
                Fixture f; rateMilliHz = rate; counterPhase = phase; extraSleepNs = extra;
                f.memory.regs[counter] = 0xfffff8;
                auto expected = f.memory.regs;
                const uint64_t elapsed = 2000000000ULL + extra;
                const uint32_t advance = uint32_t(int64_t(elapsed * rate / 1000000000000ULL) + phase);
                expected[counter] = (expected[counter] + advance) & 0xffffff;
                f.measureRefresh();
                check(f.fPanelTimingValid && f.fRefreshMilliHz == rate);
                check(f.fFrameNs == 1000000000000ULL / rate);
                check(f.fPanelTiming.pixelClock == (2720ULL * 1694 * rate + 500) / 1000);
                check(sleeps == 1 && sleptMilliseconds == 2000 && f.memory.releases == 1);
                check(f.memory.regs == expected);
            }
        }
    }
    Fixture nonstandard; rateMilliHz = 130000; nonstandard.measureRefresh();
    check(nonstandard.fPanelTimingValid && nonstandard.fRefreshMilliHz == 130000);
    std::printf("actual refresh/timing callbacks: %u CPU assertions passed\n", checks);
}
