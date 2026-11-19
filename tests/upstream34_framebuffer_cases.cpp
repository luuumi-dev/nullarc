/* CPU doubles around extracted PhxAMDFB methods. No IOKit, MMIO or GPU calls.
 * Generated production method bodies retain their component licence. */
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "PhxHardwarePolicy.hpp"
#include "nvrm_vram_abi.h"

using UInt32 = uint32_t;
using SInt64 = int64_t;
using IOReturn = int;
constexpr int kIOReturnSuccess = 0, kIOReturnBadArgument = 1;
constexpr unsigned PAGE_SIZE = 4096, kIODirectionInOut = 1, kIOMemoryPhysicallyContiguous = 2;
constexpr unsigned kIOMemoryMapperNone = 0, kIOMapWriteCombineCache = 1;
constexpr int kernel_task = 0, gIOServicePlane = 0;
#define PFBLOG(...) ((void)0)
#define OSDynamicCast(Type, object) dynamic_cast<Type *>(object)
#define OSSafeReleaseNULL(object) do { if (object) (object)->release(); (object) = nullptr; } while (0)
static bool OSCompareAndSwap(UInt32 old, UInt32 value, volatile UInt32 *ptr) {
    return __sync_bool_compare_and_swap(ptr, old, value);
}
static SInt64 OSAddAtomic64(SInt64 delta, volatile SInt64 *ptr) {
    const auto old = *ptr; *ptr += delta; return old;
}
struct OSObject { virtual ~OSObject() = default; unsigned releases = 0; void release() { ++releases; } };
struct OSNumber : OSObject { uint64_t value = 0; uint64_t unsigned64BitValue() const { return value; } };
struct IORegistryEntry : OSObject { OSObject *property = nullptr; OSObject *getProperty(const char *) { return property; } };
struct OSIterator : OSObject {
    OSObject *entry = nullptr; bool used = false;
    OSObject *getNextObject() { if (used) return nullptr; used = true; return entry; }
};
struct IOMemoryMap : OSObject {
    std::array<uint32_t, 64> pixels{};
    uintptr_t getVirtualAddress() { return reinterpret_cast<uintptr_t>(pixels.data()); }
};
static IOMemoryMap candidate;
static unsigned rangeCalls, mapFailures, rangeFailures;
static uint64_t lastPhysical, lastBytes;
struct IODeviceMemory : OSObject {
    uint64_t physical = 0x4000, length = 0x10000;
    uint64_t getPhysicalAddress() { return physical; }
    uint64_t getLength() { return length; }
    IOMemoryMap *map(unsigned) { if (mapFailures) { --mapFailures; return nullptr; } return &candidate; }
    static IODeviceMemory *withRange(uint64_t physical, uint64_t bytes);
};
static IODeviceMemory aliasDevice;
IODeviceMemory *IODeviceMemory::withRange(uint64_t physical, uint64_t bytes) {
    ++rangeCalls; lastPhysical = physical; lastBytes = bytes;
    if (rangeFailures) { --rangeFailures; return nullptr; }
    return &aliasDevice;
}
struct MockPCI {
    OSIterator iterator; std::vector<IODeviceMemory *> devices;
    OSIterator *getChildIterator(int) { iterator.used = false; return &iterator; }
    uint32_t getDeviceMemoryCount() { return static_cast<uint32_t>(devices.size()); }
    IODeviceMemory *getDeviceMemoryWithIndex(uint32_t i) { return devices.at(i); }
};
struct IOSubMemoryDescriptor : OSObject { uint64_t length = 4096; uint64_t getLength() { return length; } };
static unsigned allocationCalls, allocationFailures, prepareFailures, allocatorFailures;
struct IOBufferMemoryDescriptor : OSObject {
    uint64_t length = 4096; unsigned completes = 0;
    IOReturn prepare() { if (prepareFailures) { --prepareFailures; return kIOReturnBadArgument; } return kIOReturnSuccess; }
    void complete() { ++completes; }
    uint64_t getLength() { return length; }
    void *getBytesNoCopy() { return reinterpret_cast<void *>(0x100000); }
    uint64_t getPhysicalSegment(unsigned, void *, unsigned) { return 0x100000; }
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(int, unsigned, uint64_t, uint64_t);
};
static std::array<IOBufferMemoryDescriptor, 3> buffers;
IOBufferMemoryDescriptor *IOBufferMemoryDescriptor::inTaskWithPhysicalMask(int, unsigned, uint64_t bytes, uint64_t) {
    auto &buffer = buffers.at(allocationCalls++); buffer.length = bytes;
    if (allocationFailures) { --allocationFailures; return nullptr; } return &buffer;
}
struct IORangeAllocator : OSObject {
    enum { kLocking = 1 };
    unsigned frees = 0; uint64_t offset = 0, bytes = 0;
    void deallocate(uint64_t o, uint64_t b) { ++frees; offset = o; bytes = b; }
    static IORangeAllocator *withRange(uint64_t, unsigned, unsigned, unsigned);
};
static IORangeAllocator allocator;
IORangeAllocator *IORangeAllocator::withRange(uint64_t, unsigned, unsigned, unsigned) {
    if (allocatorFailures) { --allocatorFailures; return nullptr; } return &allocator;
}

struct Fixture {
    OSNumber number; IORegistryEntry entry; MockPCI pci; IODeviceMemory bar; IOMemoryMap scanout;
    MockPCI *fPCI = &pci;
    IOMemoryMap *fSurfMap = &scanout, *fAliasMap = nullptr;
    unsigned fAliasTries = 0, fAliasWaits = 0;
    volatile UInt32 fAliasBusy = 0;
    uint64_t fBase = 0x5000;
    unsigned fRowBytes = 64, fHeight = 4;
    IOBufferMemoryDescriptor *fPool = nullptr;
    IORangeAllocator *fPoolRange = &allocator;
    uint8_t *fPoolKva = reinterpret_cast<uint8_t *>(0x100000);
    uint64_t fPoolBytes = 8192, fPoolPhys = 0;
    volatile SInt64 fVramBytes = 8192;
    static constexpr uint64_t kPoolTry[3] = {512ull << 20, 256ull << 20, 128ull << 20};
    Fixture() { number.value = 0x100000; entry.property = &number; pci.iterator.entry = &entry; pci.devices = {&bar}; }
    // ACTUAL_ALIAS_METHOD
    // ACTUAL_POOL_INIT
    // ACTUAL_POOL_FREE
};

static unsigned checks = 0;
static void check(bool value) { assert(value); ++checks; }
static void reset() {
    rangeCalls = mapFailures = rangeFailures = 0; lastPhysical = lastBytes = 0;
    candidate = IOMemoryMap{}; aliasDevice = IODeviceMemory{};
    allocationCalls = allocationFailures = prepareFailures = allocatorFailures = 0;
    buffers = {}; allocator = IORangeAllocator{};
}
static void alias_cases() {
    reset(); Fixture wait; wait.number.value = 0;
    for (unsigned i = 0; i < 5; ++i) check(wait.aliasMap() == nullptr);
    check(wait.fAliasTries == 0 && wait.fAliasWaits == 5 && rangeCalls == 0 && wait.fAliasBusy == 0);
    wait.number.value = 0x100000; candidate.pixels.fill(7); wait.scanout.pixels.fill(7);
    check(wait.aliasMap() == &candidate);
    check(wait.fAliasTries == 1 && lastPhysical == 0x101000 && lastBytes == 256 && rangeCalls == 1);
    check(wait.aliasMap() == &candidate && rangeCalls == 1);

    reset(); Fixture noBar; noBar.pci.devices.clear();
    check(noBar.aliasMap() == nullptr && rangeCalls == 0 && noBar.fAliasTries == 0 && noBar.fAliasWaits == 1);
    Fixture overflow; overflow.number.value = UINT64_MAX;
    check(overflow.aliasMap() == nullptr && rangeCalls == 0 && overflow.fAliasTries == 0);
    Fixture busy; busy.fAliasBusy = 1;
    check(busy.aliasMap() == nullptr && busy.fAliasTries == 0 && busy.fAliasWaits == 0);

    reset(); Fixture mismatch; candidate.pixels.fill(9); mismatch.scanout.pixels.fill(9); candidate.pixels[7] = 8;
    for (unsigned i = 0; i < 4; ++i) check(mismatch.aliasMap() == nullptr);
    check(rangeCalls == 3 && candidate.releases == 3 && mismatch.fAliasTries == 3 && mismatch.fAliasBusy == 0);
    reset(); Fixture zero;
    check(zero.aliasMap() == nullptr && candidate.releases == 1 && zero.fAliasTries == 1);
    reset(); Fixture mapping; mapFailures = 4;
    for (unsigned i = 0; i < 4; ++i) check(mapping.aliasMap() == nullptr);
    check(rangeCalls == 3 && mapping.fAliasTries == 3 && aliasDevice.releases == 3);
    reset(); Fixture range; rangeFailures = 1;
    check(range.aliasMap() == nullptr && range.fAliasTries == 1 && range.fAliasBusy == 0);
    reset(); Fixture exhausted; exhausted.number.value = 0;
    for (unsigned i = 0; i < 3601; ++i) check(exhausted.aliasMap() == nullptr);
    check(exhausted.fAliasWaits == 3600 && exhausted.fAliasTries == 0 && rangeCalls == 0);
    exhausted.number.value = 0x100000;
    check(exhausted.aliasMap() == nullptr && rangeCalls == 0);
}
static void pool_cases() {
    reset(); Fixture f; allocatorFailures = 1;
    f.poolInit();
    check(allocationCalls == 2 && buffers[0].completes == 1 && buffers[0].releases == 1);
    check(f.fPool == &buffers[1] && f.fPoolBytes == (256ull << 20));
    reset(); Fixture missing; allocationFailures = 3; missing.poolInit();
    check(allocationCalls == 3 && missing.fPool == nullptr);
    reset(); Fixture preparation; prepareFailures = 1; preparation.poolInit();
    check(allocationCalls == 2 && buffers[0].releases == 1 && preparation.fPool == &buffers[1]);

    reset(); Fixture free;
    IOSubMemoryDescriptor sub; NVRMVramRequest request{};
    request.version = NVRM_VRAM_ABI_VERSION; request.handle = &sub;
    const uintptr_t bad[] = {0, 0x0fffff, 0x100001, 0x102000, UINTPTR_MAX};
    for (auto address : bad) {
        request.kva = reinterpret_cast<void *>(address);
        check(free.vramFree(&request) == kIOReturnBadArgument);
    }
    check(allocator.frees == 0 && free.fVramBytes == 8192 && sub.releases == 0);
    request.kva = reinterpret_cast<void *>(0x100000); sub.length = 0;
    check(free.vramFree(&request) == kIOReturnBadArgument);
    sub.length = 4097; check(free.vramFree(&request) == kIOReturnBadArgument);
    sub.length = UINT64_MAX; check(free.vramFree(&request) == kIOReturnBadArgument);
    sub.length = 4096; request.kva = reinterpret_cast<void *>(0x101000);
    check(free.vramFree(&request) == kIOReturnSuccess);
    check(allocator.frees == 1 && allocator.offset == 4096 && allocator.bytes == 4096 && sub.releases == 1 && free.fVramBytes == 4096);
    check(free.vramFree(nullptr) == kIOReturnBadArgument);
    request.version = 0; check(free.vramFree(&request) == kIOReturnBadArgument);
    request.version = NVRM_VRAM_ABI_VERSION; free.fPoolKva = nullptr;
    check(free.vramFree(&request) == kIOReturnBadArgument);
    reset(); Fixture direct; IOBufferMemoryDescriptor standalone; request.handle = &standalone;
    check(direct.vramFree(&request) == kIOReturnSuccess && standalone.releases == 1 && standalone.completes == 1);
}
int main() { alias_cases(); pool_cases(); std::printf("actual framebuffer methods: %u CPU assertions passed\n", checks); }
