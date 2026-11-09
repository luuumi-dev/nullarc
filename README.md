<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/brand/nullarc-logo-dark.svg">
    <img src="assets/brand/nullarc-logo-light.svg" alt="nullarc" width="420">
  </picture>
</p>

<p align="center">
  <b>Metal on AMD Radeon 780M, 760M & 740M for macOS Tahoe.</b><br>
  GPU bring-up, display compositor driver for WindowServer at 120 Hz, and Metal over Vulkan (Mesa RADV)<br>
  for Phoenix / Hawk Point APUs (RDNA 3, GC 11.0.1, gfx1103).
</p>

<p align="center">
  <img alt="macOS Tahoe 26" src="https://img.shields.io/badge/macOS-Tahoe%2026-0B0D12?logo=apple&logoColor=white">
  <img alt="RDNA3 gfx1103" src="https://img.shields.io/badge/GPU-Radeon%20780M%20%2F%20760M%20%2F%20740M-FF3D2E?logo=amd&logoColor=white">
  <img alt="Metal over Vulkan" src="https://img.shields.io/badge/Metal-over%20Vulkan%20(RADV)-7C5CFF">
  <a href="https://github.com/luuumi-dev/nullarc/releases"><img alt="Latest Release" src="https://img.shields.io/badge/release-v0.4.0--alpha-blue"></a>
  <a href="https://github.com/luuumi-dev/nullarc/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/luuumi-dev/nullarc/actions/workflows/ci.yml/badge.svg"></a>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-multi-FF2D87"></a>
</p>

<p align="center"><b>English</b> · <a href="README_RU.md">Русский</a></p>

---

## Status

Measured on the reference machine — Firebat U6C, AMD Ryzen 7 7840HS / H 255, Radeon 780M (12 CUs), 2560×1600 120 Hz eDP, macOS Tahoe 26.6.2 (November 2026):

| Feature | State | Notes |
|---|---|---|
| **Desktop Compositing** | Working | WindowServer composites on GPU at 120 Hz; dragging windows and CoreAnimation stay pinned at 116–120 fps. |
| **Metal 3D Graphics** | Working | SceneKit 3D benchmarks achieve **117.2 FPS** at 2400×1500 (1.81 ms CPU/frame). Real rasterization validated via `mtlprobe render`. |
| **Metal Compute** | Working | Parallel compute verified (`mtlprobe`: 4096/4096 parallel workgroups return mathematically exact results). |
| **Performance Level** | Tier | Matches or exceeds Apple M2 (10-core GPU) / AMD Radeon Pro 5500M in raw compute; ~4–5× faster than Intel Iris Plus G7. |
| **Liquid Glass & Lensing** | Working | Dock, Control Center, widgets, and in-app glass drawn by GPU using Apple's compositor shaders (`glass_background_sdf_lpf` with SDF curvature). |
| **Display & Backlight** | Working | Native brightness slider in Control Center and Fn hotkeys work via dual PWM in `PhxAMDFB` (AppleBacklightDisplay). |
| **Audio** | Working | `AppleHDA` injected through `PhxHDA` (`alcid=18`) for built-in speakers and microphone. |
| **Night Shift** | Working | Color temperature adjustments apply smoothly via `kFramebufferSupportsGammaCorrection` and IORegistry gamma descriptors without CPU thrashing. |
| **Kernel Stability** | Working | Boot-time CPU spin in `doWaitSeq` replaced with adaptive backoff; no watchdog freezes or startup stalls. |
| **Sleep / Wake** | In Progress | Display sleep & wake fully functional; S4 Hibernation power management registered in IOKit (`PMinit` / `setPowerState`). |
| **Hardware Support** | Unified | Dynamic topology engine supports Radeon 780M (12 CUs), 760M (8 CUs), and 740M (4 CUs) via selector 30. |
| **Not yet** | WIP | External display PHYs (HDMI / Type-C DP via DCN 3.1.4 in progress), hardware VCN 4.0.2 video decode. |

Known bugs, architecture notes, and progress are tracked in [KNOWN_ISSUES.md](KNOWN_ISSUES.md) and [BUGS.md](BUGS.md).

## How it works

```mermaid
flowchart TD
  apps["Apps · WindowServer"] -->|Metal| plugin["NVMTLDriver.bundle<br/>Metal objects on Vulkan"]
  plugin -->|"AIR → SPIR-V"| xlate["libnvmtl_translate"]
  plugin -->|Vulkan| radv["Mesa RADV<br/>libvulkan_radeon.dylib"]
  plugin -->|IOAccelerator| accel["PhxAccel.kext<br/>NMXAccel · PhxAMDFB · PhxAGDC"]
  radv -->|"IOKit user client"| bringup["PhxBringup.kext<br/>PSP · MES · GPUVM"]
  bringup --> gpu[("RDNA3 APU<br/>780M / 760M / 740M")]
  accel -->|scan-out| panel["eDP panel · DCN 3.1.4"]
```

| Component | Installed in | Source | Role |
|---|---|---|---|
| `PhxBringup.kext` | OpenCore `EFI/OC/Kexts` | [`src/phxbringup`](src/phxbringup) | Loads firmware through PSP, initializes GFX (SMU/IMU), runs MES scheduler, manages per-process GPUVM address spaces, and implements selector 30 topology queries and fence wait queues. |
| `PhxAccel.kext` | Auxiliary kernel collection | [`src/phxaccel`](src/phxaccel) | `NMXAccel` IOAccelerator, `PhxAMDFB` framebuffer (VRAM pool, 120 Hz vblank, hardware gamma, dual PWM backlight) and `PhxAGDC` display timing. |
| `NVMTLDriver.bundle` | `/System/Library/.../PhxGPU`, linked from `/Library/GPUBundles` | [`src/nmplugin`](src/nmplugin) | Metal driver plugin: Metal over Vulkan, zero-copy IOSurfaces, shader disk caching, and MTLSimDriver texture buffer alignment. |
| `libvulkan_radeon.dylib` | `PhxGPU/nvmtl` | Mesa + [`mesa-patches-phoenix`](mesa-patches-phoenix) | RADV for gfx1103 with Darwin winsys and captured Phoenix/Hawk Point topology support (Patch 0007). |
| `phxboot` | LaunchDaemon `local.phxboot` | [`tools/phxctl/phxboot.sh`](tools/phxctl/phxboot.sh) | Initializes GPU at boot before WindowServer opens the panel, with automatic watchdog fail-safes. |

## Installing

Download the latest release package (`nullarc-v0.4.0-alpha.zip`) from the [Releases page](https://github.com/luuumi-dev/nullarc/releases) and follow **[docs/INSTALL.md](docs/INSTALL.md)**.

1. Add `PhxBringup.kext` and recommended boot-args (`phxdebug=1`, `amfi=0x80`, `amfi_get_out_of_my_way=0x1`) to OpenCore `config.plist`.
2. Copy `hardware/acpi/SSDT-ALIB.aml` to `EFI/OC/ACPI/` to prevent USB-C power event ACPI stalls.
3. Boot into macOS and run:
   ```sh
   sudo ./install.sh --windowserver
   ```
4. Reboot twice as prompted by the installer.

## Building and Testing

On the Mac (Command Line Tools, [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK)):

```sh
sh src/phxbringup/build.sh /path/to/MacKernelSDK      # Builds PhxBringup.kext
sh src/phxaccel/build.sh /path/to/MacKernelSDK        # Builds PhxAccel.kext
cd src/nmplugin && ./build.sh                        # Builds NVMTLDriver.new
```

To run the complete host unit test suite:

```sh
make test
python3 tests/test_phx_profile.py
```

The test runner validates:
1. `test_rdna3_discovery_uma`: IP discovery & UMA ReBAR configuration
2. `test_rdna3_ip_dcn`: DCN 3.1.4 pixel timings and IP block bring-up
3. `test_rdna3_ucode`: GFX1103 ucode extractor with adversarial fuzzer
4. `test_r3n_abi`: UserClient ABI handshake, BO allocation, PM4 CS submission
5. `test_phx_topology`: 128-byte wire topology and CU/RB masks for 780M, 760M, and 740M
6. `test_wait_seq`: Non-spinning adaptive wait queues, timeout clamping, and fence retirement

## Notable Fixes

- **Eliminated Kernel Boot Spin**: Replaced CPU tight-spin `IODelay(100)` in `doWaitSeq` with adaptive scheduler sleep (`IOSleep(1)` for waits ≥ 1 ms), removing boot-time freezing.
- **Unified 780M / 760M / 740M Topology**: Added selector 30 `PHXN_SEL_TOPOLOGY` wire format in `PhxBringup` and Mesa patch 0007, dynamically adjusting ACO limits and CU active numbers.
- **iOS Simulator Fix (B-33)**: Added `-minimumTextureBufferAlignmentForPixelFormat:` in `NVMTLDevice.m`, stopping `MercuryPosterExtension` SIGFPE division-by-zero crashes.
- **Night Shift Smooth Transition (B-28)**: Enabled `kFramebufferSupportsGammaCorrection` and IORegistry gamma descriptors in `phxfb`, offloading LUT calculations from CPU to GPU.
- **USB-C Hotplug Crash Fix (B-01)**: Created `SSDT-ALIB` to gracefully handle `\_SB.ALIB(0x0C)` power events without ACPICA stack corruption.
- **Zero-copy IOSurfaces**: Direct mapping into GPU page tables drops WindowServer CPU load from 77% to under 6%.

## License and Credits

- Code written for nullarc (`src/phxbringup`, `tools/phxctl`, `tests/`): MIT / GPL-2.0-only.
- `src/phxaccel`, `src/nmplugin`: Derived from NullMoth's driver, **PolyForm Noncommercial 1.0.0** — noncommercial use only.
- `mesa-patches-phoenix`: MIT, as Mesa.

Special thanks to the Linux `amdgpu` kernel team, Mesa RADV contributors, NullMoth, [Cobalt64](https://cobalt64.com), [Acidanthera](https://github.com/acidanthera), Kreytus and DogoSoCool5.
