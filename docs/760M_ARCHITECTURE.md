# Actual source dependencies and HP 760M gaps

The root README was read before modification. Its production stack matches the
current source; several older notes describe earlier experiments and a separate
mock driver, so their success labels do not apply to HP hardware.

```text
App / WindowServer
  Metal -> src/nmplugin/NVMTLDevice.m (includes other plugin .m files)
    NVMTLLibraryLoad.m -> dlopen(libnvmtl_translate): AIR -> SPIR-V
    NVMTLObjects.m / NVMTLEventSync.m -> nvmtl_vk.c -> Vulkan resources/submit
  RADV/ACO (external Mesa f5cb8ee0 + Cobalt64 + Phoenix patches)
    ac_gpu_info.c <- AMDGPU_INFO_DEV_INFO <- ac_darwin_drm.c
      <- ac_darwin_kext.c <- IOKit (direct PhxBringup or NMXAccel proxy)
    topology selector 30 <- PhxBringup <- captured HP profile + exact PCI match
  PhxBringup.cpp: BO pages/imports -> GPUVM -> MES rings -> CP -> fences
    phxctl/bringup5.sh: PSP/firmware/SMU
    phxctl/full.sh + mesboot.c: memory hub/CP/MES queues -> adoptQueue
    claimAccel -> PCI nmxgpu-claimed -> NMXAccel
  PhxAccel:
    nvrm-accel.cpp: IOAccelerator object and native-client proxy
    phxfb.cpp: GOP framebuffer, contiguous host pool, IOSurface frame copy
    phxagdc.cpp: display control, arming and fallback
```

## Subsystem inventory

| Concern | Actual implementation | HP implications |
|---|---|---|
| PCI/attachment | `src/phxbringup/Info.plist`, `PhxBringup::start`; accelerator personalities | Device 1900 already matches HP; distinguish revision and subsystem rather than relabel all devices |
| CU/RB topology | Mesa canned `amdgpu_devices.c` originally feeds RADV; new private topology query overlays captured fields | Physical CU bitmap + RB mask needed, not only active CU count; counts feed ACO/register resource decisions through Mesa |
| Shader translation | `NVMTLLibraryLoad.m`, `NVMTLVendorCompiler.m`, `NVMTLSass*.m`; AIR library external | AIR->SPIR-V is not CU dispatch; native AMD ISA generation lives in ACO; no xlat11 subsystem exists here |
| Pipeline/resources | `nvmtl_vk.c`, `NVMTLObjects.m` -> RADV | Shared Vulkan implementation; compiler mock succeeds with captured HP topology; no evidence of HP execution |
| Power/firmware | `tools/phxctl/bringup5.sh` -> PhxBringup PSP/SMU debug selectors | Exact file offsets/sizes and raw TMR address words are Firebat-specific; HP profile cannot validate them |
| GPUVM/memory | `PhxBringup.cpp` BO allocation, imports, multi-process page tables/VMID reuse; `mesboot.c` hub setup | `VRAM_MC=0x8000000000`, `FB_PA=0x6a0000000`, aperture/default-page settings and scratch regions in tools require target evidence |
| Scheduling/commands | MES API `mes_v11_api_def.h`, `mesboot.c` MQDs/rings/CSB -> adoptQueue -> gpuSubmit/gpuRetired | Existing hardware path, not simulated completion after queue adoption; HP is deliberately prevented from using it |
| Interrupts/recovery | Submission/wait polling; boot watchdogs and WindowServer crash/hang monitoring | No demonstrated HP interrupt-driven completion/reset or suspend/resume path; no claims of complete interrupt support |
| Framebuffer/display | `src/phxaccel/accel/phxfb.cpp` reads GOP, measures OTG0, provides vblank timer; `phxagdc.cpp` controls arming | GOP geometry is not wholly fixed at 2560x1600 here; OTG0/DCN314 assumptions still need HP validation |
| Installation/build | native kext build scripts; `install/`; `tools/release/build_release.sh` packages reference machine binaries | Source builds are not automatically installed; release script copies installed assets and is not a reproducible HP build pipeline |
| Hardware evidence | `hardware/dumps/firebat_u6c` vs new `hardware/profiles/hp-760m.json` | Reference PCI/IP/trace data describe Firebat only; supplied HP capture contains topology and identity only |
| Diagnostics | `src/phxprobe`, `tools/parse_discovery.py`, `tools/linux-trace/trace.sh`, phxctl/mesboot, vkprobe/mtlprobe/iosurf | Probe/trace procedures may change power/driver state; new Linux profile collector uses read-only DRM/sysfs queries |

## Earlier experimental path

`src/kext/RDNA3Driver*` -> `src/amd/rdna3_ip_blocks*` is a separate scaffold with
placeholder initialization. `src/uapi/r3n_user_client*` and `src/winsys/radv_r3n_winsys*`
are host-side simulations, including immediate retirement. Their hardcoded
12-CU/11.0.3/2560x1600 values and passing mock tests are not the production N48N
interface or hardware evidence. `src/amd/rdna3_ip_discovery*`, firmware extractors,
and UMA planners have useful independent tests but are not all wired into
PhxBringup. The old `tools/build_kext.py` is not the README's native production build.

## Data contract and scope

`phx_topology.h` defines a versioned, fixed-width 128-byte extension (selector 30,
outside existing N48N 0..26 and debug 100..113). The kernel reports captured data only
after matching all five PCI identity fields. A ready flag is reserved for a future
validated native implementation; it is never set by the capture generator.

The Mesa patch transports that structure, validates dimensions and populations,
then copies physical CU and RB fields into `drm_amdgpu_info_device`; Mesa's existing
GFX11 mapping in `ac_gpu_info.c` derives enabled CUs and ACO constraints. PCI identity
and marketing name follow the same profile. Existing canned paths remain available
when the extension is unsupported. An explicit mock profile exercises the HP path
without changing the default 780M mock. Neither a mock nor a captured profile
validates inherited tiling/memory/firmware properties.

Header semantics were checked against the Linux UAPI `drm_amdgpu_info_device` and
GFX11 CU mapping; no hardware register/fuse encoding was inferred from these masks:
[Linux AMDGPU UAPI](https://github.com/torvalds/linux/blob/master/include/uapi/drm/amdgpu_drm.h),
[Linux GFX11 implementation](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c),
Mesa source at the build's pinned commit. Runtime proof remains required.
