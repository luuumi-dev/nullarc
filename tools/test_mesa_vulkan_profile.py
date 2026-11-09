#!/usr/bin/env python3
"""Inspect HP/legacy Vulkan properties and compile gfx1103 ACO pipelines without a GPU."""
import argparse
import os
from pathlib import Path
import subprocess

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('mesa', type=Path)
ap.add_argument('--glslang', default='glslangValidator')
a = ap.parse_args()
repo = Path(__file__).resolve().parents[1]
mesa = a.mesa.resolve()
out = repo / 'build/tests'
out.mkdir(parents=True, exist_ok=True)
subprocess.run([a.glslang, '-V', str(repo / 'tools/vkprobe/fill.comp'), '-o', str(out / 'fill.spv')], check=True)
subprocess.run(['clang', '-arch', 'x86_64', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I' + str(mesa / 'include'), str(repo / 'tests/test_mesa_vulkan_profile.c'),
                '-o', str(out / 'test_mesa_vulkan_profile')], check=True)
for profile, count in [('hp-760m', 8), ('', 12)]:
    env = dict(os.environ, RADV_DARWIN_MOCK='1', RADV_DARWIN_CANNED='phoenix',
               RADV_DARWIN_MOCK_PROFILE=profile, MESA_SHADER_CACHE_DISABLE='true')
    subprocess.run([str(out / 'test_mesa_vulkan_profile'),
                    str(mesa / 'build/src/amd/vulkan/libvulkan_radeon.dylib'), str(count), str(out / 'fill.spv')],
                   env=env, check=True, timeout=60)
