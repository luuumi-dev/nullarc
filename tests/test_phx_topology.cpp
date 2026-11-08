#include "amd/phx_hp_760m.h"
#include "phxbringup/PhxNativeABI.h"
#include <cassert>
#include <cstdio>
int main() {
    static_assert(sizeof(phxn_info) == 192, "legacy wire layout unchanged");
    static_assert(sizeof(phx_topology) == 128, "new wire layout");
    assert(phx_topology_valid(&phx_hp_760m));
    assert(phx_hp_760m.cu_bitmap[0][0] == 15 && phx_hp_760m.cu_bitmap[0][1] == 15);
    assert(phx_hp_760m.num_cu_per_sh == 6 && phx_hp_760m.enabled_rb_pipes_mask == 10);
    assert(phx_topology_matches(&phx_hp_760m, 0x1002, 0x1900, 0xca, 0x103c, 0x8c6a));
    assert(!phx_topology_matches(&phx_hp_760m, 0x1002, 0x1900, 0xba, 0x2014, 0x8001));
    assert(!phx_topology_matches(&phx_hp_760m, 0x1002, 0x1900, 0xca, 0x103c, 0x0000));
    auto p = phx_hp_760m;
    // Generic, non-contiguous physical slots must survive: no CU compaction.
    p.cu_bitmap[0][0] = 0x33; p.cu_bitmap[0][1] = 0x2d;
    assert(phx_topology_valid(&p));
    p.cu_active_number = 12; p.cu_bitmap[0][0] = p.cu_bitmap[0][1] = 0x3f;
    assert(phx_topology_valid(&p)); // Full Phoenix topology, not a 760M default.
    p.cu_active_number = 4; p.cu_bitmap[0][0] = 3; p.cu_bitmap[0][1] = 0x30;
    assert(phx_topology_valid(&p)); // Synthetic 4-CU topology, not a claimed 740M capture.
    p.cu_bitmap[1][0] = 1; assert(!phx_topology_valid(&p));
    p = phx_hp_760m; p.cu_active_number = 12; assert(!phx_topology_valid(&p));
    p = phx_hp_760m; p.cu_bitmap[0][0] = 0x40; assert(!phx_topology_valid(&p));
    p = phx_hp_760m; p.enabled_rb_pipes_mask = 16; assert(!phx_topology_valid(&p));
    p = phx_hp_760m; p.num_cu_per_sh = 32; p.num_rb_pipes = 32;
    assert(phx_topology_valid(&p)); // Boundary shifts must remain defined.
    p.flags = 0; assert(!phx_topology_valid(&p));
    p = phx_hp_760m; p.reserved[0] = 1; assert(!phx_topology_valid(&p));
    for (unsigned sel = 0; sel < 120; ++sel) {
        const bool read = sel == 0 || sel == 1 || sel == 2 || sel == 30 || sel == 102;
        assert(phx_topology_diagnostic_selector(sel) == read);
    }
    puts("[PASS] topology, physical indexing, PCI isolation, ABI and diagnostic selector gates");
}
