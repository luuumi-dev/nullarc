//
//  test_rdna3_dgpu.cpp — RDNA 3 identification for discrete GPUs (Navi 31/32/33) and the dedicated-VRAM memory plan.
//
//  Stage 1 of docs/dgpu-support.md: classification and planning only. No register-layout profile exists for a
//  discrete part yet, so chip_profile() must keep returning nullptr for it and nothing may write to the hardware.
//
#include "../src/amd/rdna3_ip_discovery.h"
#include "../src/amd/rdna3_chip_profile.h"
#include "../src/amd/rdna3_rebar_uma.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace rdna3;

namespace {

struct IpSpec {
    uint16_t hw;
    uint8_t major, minor, revision;
    uint8_t nbase;
    uint32_t bases[2];
};

// Offsets inside the synthetic directories (same layout the existing discovery test uses).
constexpr size_t kTableOffset = 0x20;
constexpr size_t kDieOffset = 0x80;

std::vector<uint8_t> make_table(const std::vector<IpSpec> &ips) {
    std::vector<uint8_t> buf(2048, 0);
    auto put16 = [&buf](size_t o, uint16_t v) { std::memcpy(&buf[o], &v, 2); };
    auto put32 = [&buf](size_t o, uint32_t v) { std::memcpy(&buf[o], &v, 4); };

    put32(0, kIpDiscoveryBinarySig);
    put16(4, 3);   // version_major 3: table_list starts at +16, as on the RDNA 3 tables
    put16(6, 0);
    put16(12, 1);  // one table
    put16(16, static_cast<uint16_t>(kTableOffset));

    put32(kTableOffset, kIpdsTableSig);
    put16(kTableOffset + 4, 3);
    put16(kTableOffset + 8, 0);   // id
    put16(kTableOffset + 12, 1);  // one die
    put16(kTableOffset + 16, static_cast<uint16_t>(kDieOffset));

    put16(kDieOffset, 0);
    put16(kDieOffset + 2, static_cast<uint16_t>(ips.size()));

    size_t p = kDieOffset + 4;
    for (const IpSpec &ip : ips) {
        put16(p, ip.hw);
        buf[p + 2] = 0;  // instance
        buf[p + 3] = ip.nbase;
        buf[p + 4] = ip.major;
        buf[p + 5] = ip.minor;
        buf[p + 6] = ip.revision;
        buf[p + 7] = 0;
        for (uint8_t b = 0; b < ip.nbase; ++b)
            put32(p + 8 + 4u * b, ip.bases[b]);
        p += 8 + 4u * ip.nbase;
    }

    const uint16_t bin_size = static_cast<uint16_t>((p + 15u) & ~15u);
    put16(10, bin_size);
    put16(20, static_cast<uint16_t>(kDieOffset - kTableOffset));  // ipd size >= 80

    uint16_t sum = 0;
    for (size_t i = 10; i < bin_size; ++i)
        sum = static_cast<uint16_t>(sum + buf[i]);
    put16(8, sum);
    return buf;
}

// Navi 33 (RX 7600): GC 11.0.2 / PSP 13.0.7 / SMU 13.0.7 / MMHUB 3.0.2 / NBIF 7.7.0 / DCN 3.2.1 / SDMA 6.0.2.
// The versions are the ones the firmware file names encode (gc_11_0_2, psp_13_0_7, smu_13_0_7, dcn_3_2_1,
// sdma_6_0_2 in linux-firmware); the capture in tools/linux-dgpu confirms them on the real card.
std::vector<IpSpec> navi33_ips() {
    return {
        {static_cast<uint16_t>(HwId::Gc), 11, 0, 2, 2, {0x00001000, 0x00002000}},
        {static_cast<uint16_t>(HwId::Mp0), 13, 0, 7, 2, {0x00003000, 0x00003800}},
        {static_cast<uint16_t>(HwId::Mp1), 13, 0, 7, 2, {0x00004000, 0x00004800}},
        {static_cast<uint16_t>(HwId::Mmhub), 3, 0, 2, 2, {0x00005000, 0x00005800}},
        {static_cast<uint16_t>(HwId::Nbif), 7, 7, 0, 3, {0x00006000, 0x00006800}},
        {static_cast<uint16_t>(HwId::Dmu), 3, 2, 1, 2, {0x00007000, 0x00007800}},
        {static_cast<uint16_t>(HwId::Sdma0), 6, 0, 2, 2, {0x00008000, 0x00008800}},
    };
}

std::vector<IpSpec> phoenix_ips() {
    return {
        {static_cast<uint16_t>(HwId::Gc), 11, 0, 1, 2, {0x00001000, 0x00002000}},
        {static_cast<uint16_t>(HwId::Mp0), 13, 0, 4, 2, {0x00003000, 0x00003800}},
        {static_cast<uint16_t>(HwId::Mp1), 13, 0, 4, 2, {0x00004000, 0x00004800}},
        {static_cast<uint16_t>(HwId::Mmhub), 3, 0, 1, 2, {0x00005000, 0x00005800}},
        {static_cast<uint16_t>(HwId::Nbif), 7, 7, 0, 3, {0x00006000, 0x00006800}},
        {static_cast<uint16_t>(HwId::Dmu), 3, 1, 4, 2, {0x00007000, 0x00007800}},
        {static_cast<uint16_t>(HwId::Sdma0), 6, 0, 1, 2, {0x00008000, 0x00008800}},
    };
}

// Replaces the GC entry's revision, keeping every other block intact.
std::vector<IpSpec> with_gc_revision(std::vector<IpSpec> ips, uint8_t revision) {
    for (IpSpec &ip : ips)
        if (ip.hw == static_cast<uint16_t>(HwId::Gc))
            ip.revision = revision;
    return ips;
}

// A synthetic table plus the parser that reads it: the parser keeps a pointer into `buf`, so both live together.
struct Table {
    std::vector<uint8_t> buf;
    IpDiscoveryParser parser;

    explicit Table(const std::vector<IpSpec> &ips) : buf(make_table(ips)) {
        const bool ok = parser.init(buf.data(), buf.size());
        assert(ok);
        assert(parser.is_valid());
    }

    ChipIdentity identity() const { return chip_identity(parser); }
};

void test_navi33_identity() {
    const Table t(navi33_ips());
    const ChipIdentity id = t.identity();

    assert(id.family == ChipFamily::Navi33);
    assert(id.kind == ChipKind::Discrete);
    assert(!chip_is_apu(id.family));
    assert(id.has_display);
    assert(id.gc.major == 11 && id.gc.minor == 0 && id.gc.revision == 2);
    assert(id.mp0.major == 13 && id.mp0.revision == 7);
    assert(std::strcmp(chip_family_name(id.family), "Navi 33") == 0);

    // The APU gate must stay shut for a Navi table (DCN 3.2.x, not 3.1.x).
    assert(!t.parser.is_phoenix_apu());

    // Stage 1 has no register row for a discrete part: every caller keeps refusing to touch the hardware.
    assert(chip_profile(t.parser) == nullptr);

    std::cout << "[PASS] test_navi33_identity (GC 11.0.2 -> Navi 33, discrete, no register profile)" << std::endl;
}

void test_dgpu_families() {
    assert(Table(with_gc_revision(navi33_ips(), 0)).identity().family == ChipFamily::Navi31);
    assert(Table(with_gc_revision(navi33_ips(), 3)).identity().family == ChipFamily::Navi32);
    for (uint8_t rev : {uint8_t(0), uint8_t(2), uint8_t(3)})
        assert(Table(with_gc_revision(navi33_ips(), rev)).identity().kind == ChipKind::Discrete);
    // GC 11.5 (RDNA 3.5) is outside this table: Unknown, and treated as discrete so no UMA shortcut is taken.
    {
        std::vector<IpSpec> ips = navi33_ips();
        for (IpSpec &ip : ips)
            if (ip.hw == static_cast<uint16_t>(HwId::Gc))
                ip.minor = 5;
        const ChipIdentity id = Table(ips).identity();
        assert(id.family == ChipFamily::Unknown);
        assert(id.kind == ChipKind::Discrete);
    }
    std::cout << "[PASS] test_dgpu_families (Navi 31/32/33 split, unknown GC refused)" << std::endl;
}

void test_apu_path_unchanged() {
    const Table t(phoenix_ips());
    const ChipIdentity id = t.identity();

    assert(id.family == ChipFamily::Phoenix1);
    assert(id.kind == ChipKind::Apu);
    assert(chip_is_apu(id.family));

    const ChipProfile *prof = chip_profile(t.parser);
    assert(prof != nullptr);
    assert(prof->kind == ChipKind::Apu);
    assert(prof->regs_verified);

    ChipRegs regs = {};
    assert(chip_regs(t.parser, *prof, regs));
    // mp_13_0_4_offset.h: MP0 C2PMSG_58 = 0x7a in segment 1 of the MP0 block.
    assert(regs.psp_c2pmsg_58 == 0x3800u + 0x007au);
    // smu_v13_0_4_ppt.c: MP1 C2PMSG_66 = 0x282 in segment 1.
    assert(regs.smu_msg == 0x4800u + 0x0282u);

    std::cout << "[PASS] test_apu_path_unchanged (Phoenix profile and mailbox regs intact)" << std::endl;
}

// An unverified row may be matched but must never yield register offsets.
void test_unverified_row_refused() {
    ChipProfile prof = {};
    prof.regs_verified = false;
    prof.psp_mbox_base_idx = 0;
    const Table t(phoenix_ips());
    ChipRegs regs = {};
    assert(!chip_regs(t.parser, prof, regs));
    std::cout << "[PASS] test_unverified_row_refused (regs_verified=false -> no register writes)" << std::endl;
}

MemoryPlanInput dgpu_input() {
    MemoryPlanInput in = {};
    in.is_apu = false;
    in.bar0_len = kLegacyBar0Size;          // 256 MiB window, as on a stock RX 7600 without ReBAR
    in.bar0_addr = 0x700000000ull;
    in.bar0_cfg_addr = 0x700000000ull;
    in.rebar_present = false;
    in.rebar_bytes = 0;
    in.vram_bytes = 8ull * kGiB;            // 8 GB GDDR6
    in.tmr_reserve = kDefaultTmrReserveBytes;
    in.scanout_reserve = kDefaultDcnScanoutBytes;
    in.console_off = ~0ull;
    in.console_len = 0;
    in.map_cap = 16ull * kGiB;
    return in;
}

constexpr uint64_t kConsoleLen = 1920ull * 1080ull * 4ull;  // one GOP surface

void test_dgpu_legacy_bar() {
    MemoryPlanInput in = dgpu_input();
    in.console_off = 0;                  // the VBIOS puts the boot console at VRAM offset 0
    in.console_len = kConsoleLen;

    const MemoryPlanOutput out = plan_memory_aperture(in);
    assert(out.ok);
    assert(out.status == RebarStatus::Ok);
    assert(!out.is_full_rebar);
    assert(out.map_len == kLegacyBar0Size);
    assert(out.vram_limit == kLegacyBar0Size);

    // The scanout reserve (64 MiB) already covers the ~7.9 MiB console, so the pool still starts there.
    assert(out.vis_pool_base == kDefaultDcnScanoutBytes);
    assert(out.vis_pool_size == kLegacyBar0Size - kDefaultDcnScanoutBytes);

    // Everything above the BAR window can only be reached through GPUVM.
    assert(out.has_hi_pool);
    assert(out.hi_pool_base == kLegacyBar0Size);
    assert(out.hi_pool_size == 8ull * kGiB - kDefaultTmrReserveBytes - kLegacyBar0Size);
    assert(out.aperture_pts[2] == out.vram_limit - kPageTableAlign);

    std::cout << "[PASS] test_dgpu_legacy_bar (256 MiB window, 8 GiB VRAM, 7.75 GiB high pool)" << std::endl;
}

void test_dgpu_console_above_scanout() {
    MemoryPlanInput in = dgpu_input();
    in.console_off = 128ull * kMiB;       // a firmware that placed its surface higher in the window
    in.console_len = kConsoleLen;

    const MemoryPlanOutput out = plan_memory_aperture(in);
    assert(out.ok);
    // 128 MiB + 7.9 MiB, rounded up to the 64 KiB pool alignment: the pool starts above the console, not at 64 MiB.
    const uint64_t console_end = (128ull * kMiB + kConsoleLen + (kPageTableAlign - 1ull)) & ~(kPageTableAlign - 1ull);
    assert(console_end == 2175ull * kPageTableAlign);
    assert(out.vis_pool_base == console_end);
    assert(out.vis_pool_size == kLegacyBar0Size - out.vis_pool_base);

    // Console below the scanout reserve does not move the floor.
    MemoryPlanInput low = dgpu_input();
    low.console_off = 8ull * kMiB;
    low.console_len = kConsoleLen;
    const MemoryPlanOutput low_out = plan_memory_aperture(low);
    assert(low_out.ok);
    assert(low_out.vis_pool_base == kDefaultDcnScanoutBytes);

    std::cout << "[PASS] test_dgpu_console_above_scanout (console carved out of the visible pool)" << std::endl;
}

void test_dgpu_full_rebar() {
    MemoryPlanInput in = dgpu_input();
    in.bar0_len = 8ull * kGiB;
    in.bar0_addr = 0x800000000ull;
    in.bar0_cfg_addr = 0x800000000ull;
    in.rebar_present = true;
    in.rebar_bytes = 8ull * kGiB;
    in.console_off = 0;
    in.console_len = kConsoleLen;

    const MemoryPlanOutput out = plan_memory_aperture(in);
    assert(out.ok);
    assert(out.is_full_rebar);
    assert(!out.has_hi_pool);
    assert(out.vram_limit == 8ull * kGiB - kDefaultTmrReserveBytes);
    assert(out.vis_pool_size == out.vram_limit - kDefaultDcnScanoutBytes);

    std::cout << "[PASS] test_dgpu_full_rebar (8 GiB BAR exposes all usable VRAM)" << std::endl;
}

void test_dgpu_map_cap_and_errors() {
    // A mapping cap smaller than an 8 GiB ReBAR window truncates it, the rest becomes a high pool.
    MemoryPlanInput in = dgpu_input();
    in.bar0_len = 8ull * kGiB;
    in.bar0_addr = 0x800000000ull;
    in.rebar_present = true;
    in.rebar_bytes = 8ull * kGiB;
    in.map_cap = 512ull * kMiB;
    MemoryPlanOutput out = plan_memory_aperture(in);
    assert(out.ok);
    assert(out.map_len == 512ull * kMiB);
    assert(out.vram_limit == 512ull * kMiB);
    assert(out.hi_pool_size == 8ull * kGiB - kDefaultTmrReserveBytes - 512ull * kMiB);

    // 64 MiB window with a 64 MiB scanout reserve leaves nothing visible.
    MemoryPlanInput tiny = dgpu_input();
    tiny.bar0_len = 64ull * kMiB;
    tiny.bar0_addr = 0x700000000ull;
    out = plan_memory_aperture(tiny);
    assert(!out.ok);
    assert(out.status == RebarStatus::PoolFloorBeyondLimit);
    assert(std::strcmp(rebar_status_name(out.status), "Scanout/console reserve leaves no visible VRAM pool") == 0);

    // ReBAR-reported size must match BAR0 for a discrete card too.
    MemoryPlanInput mismatch = dgpu_input();
    mismatch.bar0_len = 256ull * kMiB;
    mismatch.rebar_present = true;
    mismatch.rebar_bytes = 512ull * kMiB;
    out = plan_memory_aperture(mismatch);
    assert(!out.ok && out.status == RebarStatus::RebarMismatch);

    std::cout << "[PASS] test_dgpu_map_cap_and_errors (map cap, empty pool, ReBAR mismatch)" << std::endl;
}

void test_apu_ignores_console_floor() {
    MemoryPlanInput in = {};
    in.is_apu = true;
    in.bar0_len = 4ull * kGiB;
    in.bar0_addr = 0x800000000ull;
    in.rebar_present = true;
    in.rebar_bytes = 4ull * kGiB;
    in.vram_bytes = 4ull * kGiB;
    in.tmr_reserve = kDefaultTmrReserveBytes;
    in.scanout_reserve = kDefaultDcnScanoutBytes;
    in.console_off = 0;                  // on an APU the console lives inside the scanout reserve by construction
    in.console_len = kConsoleLen;
    in.map_cap = 4ull * kGiB;

    const MemoryPlanOutput out = plan_memory_aperture(in);
    assert(out.ok);
    assert(out.vis_pool_base == kDefaultDcnScanoutBytes);   // unchanged from the pre-dGPU behaviour
    std::cout << "[PASS] test_apu_ignores_console_floor (APU layout byte-identical)" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running RDNA3 Discrete GPU Identification & VRAM Plan Tests ===" << std::endl;
    test_navi33_identity();
    test_dgpu_families();
    test_apu_path_unchanged();
    test_unverified_row_refused();
    test_dgpu_legacy_bar();
    test_dgpu_console_above_scanout();
    test_dgpu_full_rebar();
    test_dgpu_map_cap_and_errors();
    test_apu_ignores_console_floor();
    std::cout << "=== All tests PASSED successfully ===" << std::endl;
    return 0;
}
