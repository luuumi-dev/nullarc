/* SPDX-License-Identifier: MIT
 * Captured DRM topology, shared by the kext and RADV. No register/fuse encoding.
 * Private selector 30 does not overlap N48N scanout selectors 9..26.
 */
#ifndef PHX_TOPOLOGY_H
#define PHX_TOPOLOGY_H
#include <stdint.h>
#define PHX_SEL_TOPOLOGY 30u
#define PHX_TOPOLOGY_VERSION 1u
#define PHX_TOPOLOGY_CAPTURED 1u
/* Captured metadata alone never establishes readiness. */
#define PHX_TOPOLOGY_RUNTIME_READY 2u
struct phx_topology {
    uint32_t version, flags;
    uint32_t vendor, device, revision, subvendor, subdevice;
    uint32_t num_shader_engines, num_shader_arrays_per_engine, num_cu_per_sh;
    uint32_t num_rb_pipes, enabled_rb_pipes_mask, cu_active_number;
    uint32_t cu_bitmap[4][4];
    uint32_t reserved[3];
};
static inline int phx_topology_valid(const struct phx_topology *p) {
    if (!p || p->version != PHX_TOPOLOGY_VERSION || !(p->flags & PHX_TOPOLOGY_CAPTURED) ||
        (p->flags & ~(PHX_TOPOLOGY_CAPTURED | PHX_TOPOLOGY_RUNTIME_READY)) ||
        p->vendor != 0x1002 || p->device > 0xffff || p->revision > 0xff ||
        p->subvendor > 0xffff || p->subdevice > 0xffff ||
        !p->num_shader_engines || p->num_shader_engines > 4 ||
        !p->num_shader_arrays_per_engine || p->num_shader_arrays_per_engine > 4 ||
        !p->num_cu_per_sh || p->num_cu_per_sh > 32 ||
        !p->num_rb_pipes || p->num_rb_pipes > 32 || !p->enabled_rb_pipes_mask ||
        p->reserved[0] || p->reserved[1] || p->reserved[2]) return 0;
    uint32_t cu_mask = 0xffffffffu >> (32 - p->num_cu_per_sh);
    uint32_t rb_mask = 0xffffffffu >> (32 - p->num_rb_pipes);
    if (p->enabled_rb_pipes_mask & ~rb_mask) return 0;
    uint32_t count = 0;
    for (unsigned se = 0; se < 4; ++se) for (unsigned sa = 0; sa < 4; ++sa) {
        uint32_t m = p->cu_bitmap[se][sa];
        if (m & ~cu_mask) return 0;
        if ((se >= p->num_shader_engines || sa >= p->num_shader_arrays_per_engine) && m) return 0;
        while (m) { m &= m - 1; ++count; }
    }
    return count && count == p->cu_active_number;
}
static inline int phx_topology_matches(const struct phx_topology *p, uint32_t vendor,
    uint32_t device, uint32_t revision, uint32_t subvendor, uint32_t subdevice) {
    return p && p->vendor == vendor && p->device == device && p->revision == revision &&
        p->subvendor == subvendor && p->subdevice == subdevice;
}
static inline int phx_topology_diagnostic_selector(uint32_t selector) {
    return selector == 0 || selector == 1 || selector == 2 ||
           selector == PHX_SEL_TOPOLOGY || selector == 102;
}
#endif
