Phoenix (Radeon 780M, gfx1103) patches on top of Cobalt64's RADV Darwin port.

Base: Mesa f5cb8ee032adabef599ae892ec4e42d796b84da9 + reference/Cobalt64/mesa-patches/0001..0009.
Apply these after them (git am, 0001..0007).  0001..0003 are what tools/mesa_phoenix_canned.py,
tools/mesa_phoenix_identity.py and tools/mesa_phoenix_service.py produce (0003: open the PhxBringup service).
0004 answers AMDGPU_INFO_TIMESTAMP from SMUIO GOLDEN_TSC (needs PhxBringup build 31): Metal GPUStartTime/GPUEndTime.
0005 opens PhxBringup through NMXAccel when sandboxed (needs the PhxAccel proxy): Metal in WebKit's GPU process.
0006 unrolls nested loops without a full optimization round per level: a new CoreAnimation compositor shader
     variant compiles in ~1 s instead of ~2 s (same code), so WindowServer stalls less on first use.

Build on the Mac (Tahoe 26.6.2, CLT 16.2, Python 3.14 venv with meson/ninja/mako/pyyaml/packaging,
glslangValidator built from source into ~/gpu/prefix):
  meson setup build --buildtype=release -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= \
    -Dllvm=disabled -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled \
    -Dgles2=disabled -Dxmlconfig=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
    -Dbuild-tests=false -Dvulkan-layers= -Dtools=
  ninja -C build

Check (no GPU, in-process mock kext):
  VK_ICD_FILENAMES=build/src/amd/vulkan/radeon_devenv_icd.x86_64.json RADV_DARWIN_MOCK=1 tools/vkprobe/vkprobe
  -> "AMD Radeon 780M Graphics (RADV PHOENIX) 1002:1900", device created, compute pipeline compiled (ACO gfx1103).

0007 adds NullArc topology selector 30 and an explicit HP mock profile. DEV_INFO's
     physical CU masks, active counts, shader-array dimensions and enabled RB mask
     come from the captured kernel profile; PCI identity follows the same data.
     Legacy kexts keep their existing fallback. Captured-only HP native use fails.
     No GB_ADDR_CONFIG, memory layout or firmware values are inferred from topology.

Alternative to applying 0007 (do not do both):
  python3 /path/to/nullarc-760m/tools/mesa_phoenix_topology.py /path/to/mesa
This generator verifies all source anchors before writing. Regenerate it and the
patch together when changing the wire header/profile. Check the shared HP header:
  python3 tools/phx_profile.py --check

Host transport tests (from the NullArc checkout):
  sh tools/test_mesa_topology.sh /path/to/mesa
Mock Vulkan properties plus gfx1103 ACO pipeline compilation (macOS, x86_64):
  python3 tools/test_mesa_vulkan_profile.py /path/to/mesa --glslang /path/to/glslangValidator
The latter explicitly checks 8-CU HP and the unchanged 12-CU legacy default, with
zero command submissions. It is not hardware execution or Metal testing.
