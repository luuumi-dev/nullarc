//
//  rdna3_rebar_uma.h — Memory aperture planner for RDNA 3 APUs (Phoenix UMA carve-out) and discrete GPUs
//  (dedicated VRAM behind BAR0/ReBAR). Freestanding pure C++17 logic, host-testable under ASan/UBSan.
//
#pragma once
#include <stdint.h>

namespace rdna3 {

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kGiB = 1ull << 30;

// Default constants for Phoenix APU (4 GiB UMA VRAM):
constexpr uint64_t kDefaultPhoenixVramBytes = 4ull * kGiB;
constexpr uint64_t kDefaultTmrReserveBytes   = 64ull * kMiB; // PSP TMR reserve at top of VRAM
constexpr uint64_t kDefaultDcnScanoutBytes   = 64ull * kMiB; // 2560x1600x4 double buffered + cursor
constexpr uint64_t kPageAlign                = 4ull << 10;   // 4 KiB GPU page
constexpr uint64_t kPageTableAlign           = 64ull << 10;  // 64 KiB block align
constexpr uint64_t kLegacyBar0Size           = 256ull * kMiB;

enum class RebarStatus : uint32_t {
    Ok = 0,
    LenZero,
    LenNotPow2,
    AddrZero,
    AddrMisaligned,
    AddrWrap,
    RebarMismatch,
    NoRebarCapAboveLegacy,
    VramTooSmall,
    ConsoleBeyondLimit,
    ConsoleOverlap,
    MapCapZero,
    PoolFloorBeyondLimit,
};

const char *rebar_status_name(RebarStatus s);

constexpr bool is_pow2(uint64_t v) {
    return v != 0ull && (v & (v - 1ull)) == 0ull;
}

struct MemoryPlanInput {
    bool     is_apu;          // true for Phoenix 780M APU, false for discrete GPU
    uint64_t bar0_len;        // Physical length of BAR0 reported by OS
    uint64_t bar0_addr;       // Physical address of BAR0
    uint64_t bar0_cfg_addr;   // BAR0 address from PCI config space
    bool     rebar_present;   // PCIe Ext Cap 0x15 found and decoded
    uint64_t rebar_bytes;     // Size decoded from ReBAR capability
    uint64_t vram_bytes;      // Total VRAM (carved UMA or dedicated VRAM)
    uint64_t tmr_reserve;     // Top-of-VRAM reserve for PSP / TMR
    uint64_t scanout_reserve; // Reserved region for DCN scanout framebuffers
    uint64_t console_off;     // Console offset within BAR0 (~0ULL if none)
    uint64_t console_len;     // Console length in bytes
    uint64_t map_cap;         // Maximum allowed BAR0 mapping length
};

struct MemoryPlanOutput {
    bool        ok;
    RebarStatus status;
    bool        is_full_rebar;     // true if BAR0 exposes 100% of usable VRAM
    uint64_t    map_len;           // Mapped length of BAR0
    uint64_t    vram_limit;        // Usable CPU-visible VRAM boundary
    uint64_t    vis_pool_base;     // Start of CPU-visible allocator pool
    uint64_t    vis_pool_size;     // Size of CPU-visible allocator pool
    bool        has_hi_pool;       // true if CPU-invisible high pool is needed
    uint64_t    hi_pool_base;      // Start of CPU-invisible high pool
    uint64_t    hi_pool_size;      // Size of high pool
    uint64_t    aperture_pts[3];   // 3 verification test points (base, mid, top)
};

// Pure calculation of the memory layout.
//   APU  (is_apu = true):  the carve-out is UMA; CPU sees it through BAR0 exactly like the firmware does and the
//                          scanout reserve starts the visible pool.
//   dGPU (is_apu = false): VRAM is dedicated. The firmware/GOP console usually sits at VRAM offset 0 and must stay
//                          outside the allocator, so the visible pool starts after it. Everything above the BAR0
//                          window (map_len) is a CPU-invisible high pool that can only be reached by GPUVM.
MemoryPlanOutput plan_memory_aperture(const MemoryPlanInput &in);

} // namespace rdna3
