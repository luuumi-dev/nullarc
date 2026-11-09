/* SPDX-License-Identifier: MIT
 * Unified DRM / GPU topology definitions for nullarc (780M, 760M, 740M).
 * Shared by PhxBringup kernel extension and RADV Mesa driver.
 */
#ifndef PHX_TOPOLOGY_H
#define PHX_TOPOLOGY_H

#include <stdint.h>

#define PHX_SEL_TOPOLOGY            30u
#define PHX_TOPOLOGY_VERSION        1u
#define PHX_TOPOLOGY_CAPTURED       1u
#define PHX_TOPOLOGY_RUNTIME_READY  2u

struct phx_topology {
    uint32_t version;
    uint32_t flags;
    uint32_t vendor;
    uint32_t device;
    uint32_t revision;
    uint32_t subvendor;
    uint32_t subdevice;
    uint32_t num_shader_engines;
    uint32_t num_shader_arrays_per_engine;
    uint32_t num_cu_per_sh;
    uint32_t num_rb_pipes;
    uint32_t enabled_rb_pipes_mask;
    uint32_t cu_active_number;
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
    for (unsigned se = 0; se < 4; ++se) {
        for (unsigned sa = 0; sa < 4; ++sa) {
            uint32_t m = p->cu_bitmap[se][sa];
            if (m & ~cu_mask) return 0;
            if ((se >= p->num_shader_engines || sa >= p->num_shader_arrays_per_engine) && m) return 0;
            while (m) { m &= m - 1; ++count; }
        }
    }
    return count && count == p->cu_active_number;
}

static inline int phx_topology_matches(const struct phx_topology *p, uint32_t vendor,
    uint32_t device, uint32_t revision, uint32_t subvendor, uint32_t subdevice) {
    return p && p->vendor == vendor && p->device == device && p->revision == revision &&
        p->subvendor == subvendor && p->subdevice == subdevice;
}

/* Default topologies for Phoenix family */
static const struct phx_topology g_phx_780m_default = {
    PHX_TOPOLOGY_VERSION, PHX_TOPOLOGY_CAPTURED | PHX_TOPOLOGY_RUNTIME_READY,
    0x1002u, 0x1900u, 0xc8u, 0x0u, 0x0u,
    1u, 2u, 6u, 4u, 0xfu, 12u,
    {
        {0x3fu, 0x3fu, 0x0u, 0x0u},
        {0x0u, 0x0u, 0x0u, 0x0u},
        {0x0u, 0x0u, 0x0u, 0x0u},
        {0x0u, 0x0u, 0x0u, 0x0u}
    },
    {0, 0, 0}
};

static const struct phx_topology g_phx_760m_default = {
    PHX_TOPOLOGY_VERSION, PHX_TOPOLOGY_CAPTURED | PHX_TOPOLOGY_RUNTIME_READY,
    0x1002u, 0x1900u, 0xcau, 0x103cu, 0x8c6au,
    1u, 2u, 6u, 4u, 0xau, 8u,
    {
        {0xfu, 0xfu, 0x0u, 0x0u},
        {0x0u, 0x0u, 0x0u, 0x0u},
        {0x0u, 0x0u, 0x0u, 0x0u},
        {0x0u, 0x0u, 0x0u, 0x0u}
    },
    {0, 0, 0}
};

static const struct phx_topology g_phx_740m_default = {
    PHX_TOPOLOGY_VERSION, PHX_TOPOLOGY_CAPTURED | PHX_TOPOLOGY_RUNTIME_READY,
    0x1002u, 0x15c8u, 0xc4u, 0x0u, 0x0u,
    1u, 2u, 4u, 2u, 0x3u, 4u,
    {
        {0x3u, 0x3u, 0x0u, 0x0u},
        {0x0u, 0x0u, 0x0u, 0x0u},
        {0x0u, 0x0u, 0x0u, 0x0u},
        {0x0u, 0x0u, 0x0u, 0x0u}
    },
    {0, 0, 0}
};

static inline const struct phx_topology *phx_find_topology(uint32_t vendor, uint32_t device,
                                                           uint32_t revision, uint32_t subvendor,
                                                           uint32_t subdevice) {
    (void)revision;
    if (vendor != 0x1002) return 0;
    if (subvendor == 0x103c && subdevice == 0x8c6a) return &g_phx_760m_default;
    if (device == 0x15c8) return &g_phx_740m_default;
    /* Default to 780M full profile for 0x1900 / 0x15bf / 0x164e */
    return &g_phx_780m_default;
}

#endif /* PHX_TOPOLOGY_H */
