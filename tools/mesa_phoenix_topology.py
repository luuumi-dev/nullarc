#!/usr/bin/env python3
"""Apply runtime topology support after Phoenix Mesa patches 0001..0006.

All anchors are checked before any file is written. This adds a private, versioned
query; old kexts retain the documented legacy canned path. HP native execution is
refused until the kernel reports runtime readiness. Mock profile is opt-in.
"""
import argparse
from pathlib import Path
from phx_profile import PROFILE, HEADER, load, render


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'expected exactly one Mesa anchor: {old[:100]!r}')
    return text.replace(old, new)


def apply(root):
    repo = Path(__file__).resolve().parents[1]
    if HEADER.read_text() != render(load(PROFILE)):
        raise ValueError('generated HP profile is stale')
    base = root / 'src/amd/common/darwin'
    paths = [base / n for n in ('ac_darwin_kext.h', 'ac_darwin_kext.c', 'ac_darwin_drm.h',
                                'ac_darwin_drm.c', 'ac_darwin_mock.c')]
    paths += [root / 'src/amd/vulkan/radv_physical_device.c', base / 'test/acd_kext_test.c']
    kh, kc, dh, dc, mock, radv, test = [p.read_text() for p in paths]
    kh = replace_once(kh, '#include "navi48_native_abi.h"', '#include "navi48_native_abi.h"\n#include "phx_topology.h"')
    kh = replace_once(kh, 'int acd_kext_query_info(struct acd_kext *k, struct n48n_info *info);',
        'int acd_kext_query_info(struct acd_kext *k, struct n48n_info *info);\nint acd_kext_query_topology(struct acd_kext *k, struct phx_topology *out);')
    kc += '''
int
acd_kext_query_topology(struct acd_kext *k, struct phx_topology *out)
{
   size_t size = sizeof(*out);
   memset(out, 0, size);
   int r = kcall(k, PHX_SEL_TOPOLOGY, NULL, 0, NULL, 0, NULL, NULL, out, &size);
   if (r) return r;
   return size == sizeof(*out) && phx_topology_valid(out) ? 0 : -EPROTO;
}
'''
    dh = replace_once(dh, '#include <stdint.h>', '#include <stdint.h>\n#include "phx_topology.h"')
    dh = replace_once(dh, 'const char *acd_get_marketing_name(acd_device_handle dev);',
                     'const char *acd_get_marketing_name(acd_device_handle dev);\nint acd_profile_identity(struct phx_topology *out);')
    dc = replace_once(dc, '#include \"ac_darwin_kext.h\"', '#include \"ac_darwin_kext.h\"\n#include \"phx_hp_760m.h\"')
    dc = replace_once(dc, '   uint32_t minor; /* ABI minor the kernel reported */',
                          '   struct phx_topology topology; /* captured, not live fuse discovery */\n   uint32_t minor; /* ABI minor the kernel reported */')
    dc = replace_once(dc, 'uint32_t *minor_out)\n{', 'uint32_t *minor_out, struct phx_topology *topology)\n{')
    anchor = '   /* GB_ADDR_CONFIG: the tiling layout'
    pos = dc.index(anchor)
    dc = dc[:pos] + '''   r = acd_kext_query_topology(kext, topology);
   if (r == -EINVAL) {
      /* Old PhxBringup/Navi48 or mock without a profile: unchanged canned path. */
      memset(topology, 0, sizeof(*topology));
   } else if (r) {
      fprintf(stderr, "radv-darwin: invalid topology query (%d)\\n", r);
      return r;
   } else if (strcmp(canned->name, "phoenix") || topology->num_shader_arrays_per_engine > 2 ||
              (!(topology->flags & PHX_TOPOLOGY_RUNTIME_READY) && !env_is_1("RADV_DARWIN_MOCK"))) {
      fprintf(stderr, "radv-darwin: captured topology is not validated native GPU initialization\\n");
      return -ENODEV;
   }

''' + dc[pos:]
    dc = replace_once(dc, 'acd_native_handshake(c->kext, canned, &c->minor)',
                          'acd_native_handshake(c->kext, canned, &c->minor, &c->topology)')
    dc = replace_once(dc, '   if (!strcmp(dev->canned->name, "phoenix"))\n      return "AMD Radeon 780M Graphics";',
                          '   const struct phx_topology *p = &dev->core->topology;\n   if (phx_topology_matches(p, phx_hp_760m.vendor, phx_hp_760m.device, phx_hp_760m.revision, phx_hp_760m.subvendor, phx_hp_760m.subdevice))\n      return "AMD Radeon 760M Graphics";\n   if (p->version) return "AMD Phoenix captured profile";\n   if (!strcmp(dev->canned->name, "phoenix"))\n      return "AMD Radeon 780M Graphics";')
    dc = replace_once(dc, '      return 0;\n   }\n   case AMDGPU_INFO_MEMORY:', '''      const struct phx_topology *p = &dev->core->topology;
      if (p->version) {
         di->device_id = p->device;
         di->pci_rev = p->revision;
         di->num_shader_engines = p->num_shader_engines;
         di->num_shader_arrays_per_engine = p->num_shader_arrays_per_engine;
         di->num_cu_per_sh = p->num_cu_per_sh;
         di->num_rb_pipes = p->num_rb_pipes;
         di->enabled_rb_pipes_mask = p->enabled_rb_pipes_mask;
         di->cu_active_number = p->cu_active_number;
         memcpy(di->cu_bitmap, p->cu_bitmap, sizeof(di->cu_bitmap));
      }
      return 0;
   }
   case AMDGPU_INFO_MEMORY:''')
    dc += '''
/* Enumeration needs PCI identity before radv_physical_device_try_create. Query
 * the same core as DEV_INFO; never infer a GPU model from the PCI device alone. */
int
acd_profile_identity(struct phx_topology *out)
{
   acd_device_handle dev;
   uint32_t major, minor;
   int r = acd_device_initialize(&major, &minor, &dev);
   if (r) return r;
   *out = dev->core->topology;
   acd_device_deinitialize(dev);
   return 0;
}
'''
    mock = replace_once(mock, '#include "ac_darwin_drm.h"', '#include "ac_darwin_drm.h"\n#include "phx_hp_760m.h"')
    mock = replace_once(mock, '   case ACD_SEL_READ_REGS: {', '''   case PHX_SEL_TOPOLOGY: {
      const char *profile = getenv("RADV_DARWIN_MOCK_PROFILE");
      if (!profile || !*profile) { kr = ACD_KR_UNSUPPORTED; goto out; }
      if (strcmp(profile, "hp-760m")) { kr = ACD_KR_NOT_PERMITTED; goto out; }
      NEED(shape(0, 0, 0, sizeof(struct phx_topology)));
      memcpy(stout, &phx_hp_760m, sizeof(phx_hp_760m));
      *stoutsz = sizeof(phx_hp_760m);
      break;
   }
   case ACD_SEL_READ_REGS: {''')
    radv = replace_once(radv, '#ifdef HAVE_AMDGPU_DARWIN\n/*',
                             '#ifdef HAVE_AMDGPU_DARWIN\n#include "common/darwin/ac_darwin_drm.h"\n/*')
    radv = replace_once(radv, '   drmDevice drm_device = {', '''   struct phx_topology profile = {0};
   int profile_result = acd_profile_identity(&profile);
   if (profile_result) return VK_SUCCESS; /* backend has logged the reason */
   if (profile.version) {
      device_info.vendor_id = profile.vendor;
      device_info.device_id = profile.device;
      device_info.subvendor_id = profile.subvendor;
      device_info.subdevice_id = profile.subdevice;
      device_info.revision_id = profile.revision;
   }
   drmDevice drm_device = {''')
    test = replace_once(test,
        'CHECK(strcmp(acd_kext_service_class(), "Navi48Bringup") == 0); /* the default is unchanged */',
        'CHECK(strcmp(acd_kext_service_class(), "PhxBringup") == 0); /* Phoenix patch 0003 changes the default */')
    for p, s in zip(paths, (kh, kc, dh, dc, mock, radv, test)):
        p.write_text(s)
    for name in ('phx_topology.h', 'phx_hp_760m.h'):
        (base / name).write_text((repo / 'src/amd' / name).read_text())
    print('applied captured topology transport, RADV DEV_INFO and PCI identity; HP mock is opt-in')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('mesa', type=Path)
    args = ap.parse_args()
    try:
        apply(args.mesa)
    except (ValueError, OSError) as e:
        ap.exit(1, f'{e}\n')
