#include "../src/amd/phx_topology.h"
#include "../src/phxbringup/PhxNativeABI.h"
#include <cassert>
#include <cstdio>

int main() {
    static_assert(sizeof(phxn_info) == 192, "legacy wire layout unchanged");
    static_assert(sizeof(phx_topology) == 128, "topology wire layout must be 128 bytes");

    // 1. Validate 780M default topology (12 CUs, 4 RBs)
    assert(phx_topology_valid(&g_phx_780m_default));
    assert(g_phx_780m_default.cu_active_number == 12);
    assert(g_phx_780m_default.num_rb_pipes == 4);
    assert(g_phx_780m_default.cu_bitmap[0][0] == 0x3f && g_phx_780m_default.cu_bitmap[0][1] == 0x3f);

    // 2. Validate 760M default topology (8 CUs, 4 RBs)
    assert(phx_topology_valid(&g_phx_760m_default));
    assert(g_phx_760m_default.cu_active_number == 8);
    assert(g_phx_760m_default.cu_bitmap[0][0] == 0xf && g_phx_760m_default.cu_bitmap[0][1] == 0xf);

    // 3. Validate 740M default topology (4 CUs, 2 RBs)
    assert(phx_topology_valid(&g_phx_740m_default));
    assert(g_phx_740m_default.cu_active_number == 4);
    assert(g_phx_740m_default.num_rb_pipes == 2);
    assert(g_phx_740m_default.cu_bitmap[0][0] == 0x3 && g_phx_740m_default.cu_bitmap[0][1] == 0x3);

    // 4. Test dynamic lookup
    // HP 760M specific match
    const phx_topology *t_hp = phx_find_topology(0x1002, 0x1900, 0xca, 0x103c, 0x8c6a);
    assert(t_hp == &g_phx_760m_default);
    assert(t_hp->cu_active_number == 8);

    // 740M match (device 0x15c8)
    const phx_topology *t_740 = phx_find_topology(0x1002, 0x15c8, 0xc4, 0x1002, 0x15c8);
    assert(t_740 == &g_phx_740m_default);
    assert(t_740->cu_active_number == 4);

    // 780M fallback match
    const phx_topology *t_780 = phx_find_topology(0x1002, 0x1900, 0xc8, 0x1002, 0x1900);
    assert(t_780 == &g_phx_780m_default);
    assert(t_780->cu_active_number == 12);

    // 5. Test invalid shapes
    phx_topology bad = g_phx_780m_default;
    bad.cu_active_number = 14; // Mismatch with bitmap popcount
    assert(!phx_topology_valid(&bad));

    bad = g_phx_780m_default;
    bad.flags = 0; // Missing CAPTURED flag
    assert(!phx_topology_valid(&bad));

    bad = g_phx_780m_default;
    bad.version = 2; // Unsupported version
    assert(!phx_topology_valid(&bad));

    puts("[PASS] test_phx_topology: 780M, 760M, and 740M unified validation & matching OK");
    return 0;
}
