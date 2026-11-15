# Dedicated RDNA 3 GPUs: what nullarc needs, and a staged plan

Status: **proposal + stage 1 implementation**. Nothing here has been validated on macOS with a discrete
card yet; every claim about the hardware comes from the Linux `amdgpu` sources named inline, from
linux-firmware file names, or from Mesa. The reference card is an **Radeon RX 7600 (Navi 33)**.

The document answers three questions:

1. what nullarc assumes today that only holds for a UMA APU,
2. what a discrete RDNA 3 card changes, per subsystem,
3. how to land it in reviewable stages without risking the APU users.

Companion material in this PR: `src/amd/rdna3_chip_profile.*` (classification), `src/amd/rdna3_rebar_uma.*`
(dedicated-VRAM plan), `tests/test_rdna3_dgpu.cpp`, and `tools/linux-dgpu/` (read-only capture).

---

## 1. Reference hardware

| | Phoenix (supported) | Navi 33 / RX 7600 (target) |
|---|---|---|
| PCI ids | `1002:1900`, `15bf`, `15c8`, `164e` | `1002:7480` (also `7481`, `7483`, `7489`, `7499`, `73f0`) |
| GC / Mesa name | GC 11.0.1, gfx1103 | GC 11.0.2, gfx1102 |
| PSP (MP0) | 13.0.4 | 13.0.7 or 13.0.10 (VBIOS dependent) |
| SMU (MP1) | 13.0.4 | 13.0.7 |
| MMHUB | 3.0.1 | 3.0.2 |
| NBIF | 7.7.0 | 7.7.0 |
| Display (DMU) | DCN 3.1.4 | DCN 3.2.1 |
| SDMA | 6.0.1 | 6.0.2 |
| VCN | 4.0.2 | 4.0.4 |
| Memory | 4–6 GiB UMA carve-out | 8 GB GDDR6, 128-bit, dedicated |
| Link | n/a (on-package) | PCIe 4.0 x8, BAR0 256 MiB or ReBAR |
| Compute | 12 CU / 4 RB (780M) | 32 CU / 8 RB |

Why Navi 33 first: it is the card in issue #23, it is the smallest RDNA 3 die, Mesa already ships a
canned `navi33` device (`src/amd/common/amdgpu_devices.c`), and every firmware file it needs exists in
linux-firmware. Navi 31/32 differ only in the IP tuple table and are cheap to add once the flow works.

The versions above are the ones encoded in the firmware file names (`gc_11_0_2`, `psp_13_0_7`/`psp_13_0_10`,
`smu_13_0_7`, `sdma_6_0_2`, `dcn_3_2_1`, `vcn_4_0_4`) and in `gmc_v11_0.c`
(`MMHUB 3.0.2 -> mmhub_v3_0_2`). `tools/linux-dgpu/collect.sh` confirms them from the card's own IP
discovery table; if the capture disagrees, the capture wins.

---

## 2. What nullarc assumes today

nullarc is built around one UMA APU. The assumptions are not spread over many files; they concentrate in
six places:

| Assumption | Where | Consequence on a dGPU |
|---|---|---|
| "VRAM" is system memory | `PhxBringup.cpp:1601` — every BO is an `IOBufferMemoryDescriptor`; `phxfb.cpp:100-127` — the VRAM pool is a physically contiguous system-memory run (128–512 MiB, cap 1 GiB) | The card has 8 GB of its own GDDR6 that is not system memory. A CPU mapping is not the same address the GPU uses. |
| Every GPU PTE is a *system* page | `PhxBringup.cpp:795` `*pte = (pa & kPtAddrMask) \| kPteSysFlags` (`kPteSysFlags` = VALID \| SYSTEM \| SNOOPED \| EXEC \| RW, mtype 4) | VRAM PTEs need the VRAM mtype and no SYSTEM bit; GTT PTEs stay as they are. One flag constant cannot serve both. |
| The framebuffer is a firmware carve-out at a fixed spot | `PhxBringup.cpp:449-480` `readFbLayout()` reads `RCC_CONFIG_MEMSIZE`, publishes `VRAM,totalMB`, treats `fbPhys` as a system carve-out; `PhxNativeABI.h:111-113` fixes `PHX_VRAM_TMR_OFF` = 20 MiB and `PHX_VRAM_FWBUF_OFF` = 64 MiB | On a dGPU the TMR is allocated by the driver in VRAM (`psp_tmr_init()`, `amdgpu_psp.c:1055`, `boot_time_tmr = false` for 13.0.7/13.0.10) at a discovery-derived address, and the console lives where the VBIOS/GOP put it. |
| The BAR0 aperture is 256 MiB and holds everything | `PhxBringup.cpp:64-70` (comment: "BAR0 is 256 MiB on this machine, the UMA carve-out 6 GiB") | Still true without ReBAR, but VRAM is 8 GB: 7.75 GB is reachable only through GPUVM. With ReBAR the whole VRAM becomes CPU-visible, and the planner must accept both. |
| One service, one GPU, one canned device | `src/phxbringup/Info.plist` `IOPCIMatch` = the four APU ids; Mesa patch 0003 opens the service by name `PhxBringup`; patch 0001 defaults the canned device to `phoenix`; `ac_darwin_drm.c` keeps one `g_core` | The kext will not even match a `1002:7480`, and two GPU nodes would collide on the service name and the canned identity. |
| Presentation is a CPU copy into the firmware console | `phxfb.cpp:633` `getConsoleInfo()`; a timer stands in for the VBL | Correct only when the dGPU is the boot VGA device with a GOP surface. Otherwise DCN 3.2.1 has to be programmed, plus DP/HDMI link training. |

`plan_memory_aperture()` already carried an `is_apu` field, but it was never read — the planner had one
behaviour for both kinds of chip. Stage 1 fixes that.

---

## 3. Feasibility, subsystem by subsystem

### 3.1 Identification — easy, done in stage 1

RDNA 3 parts are distinguishable from the IP discovery table alone, without a PCI id table:

```
GC 11.0.0 -> Navi 31   (discrete)
GC 11.0.1 -> Phoenix   (APU)
GC 11.0.2 -> Navi 33   (discrete)
GC 11.0.3 -> Navi 32   (discrete)
GC 11.0.4 -> Phoenix 2 (APU)
```

That is the same line Linux draws: `amdgpu_discovery.c` sets `AMD_IS_APU` for GC 11.0.1 and 11.0.4 only.
`chip_identity()` implements it and returns `ChipKind::Discrete` for anything it does not recognise, so an
unknown part is logged and then left alone.

A discrete part still gets **no register profile** in stage 1: `chip_profile()` returns `nullptr`, and
`chip_regs()` refuses any row whose `regs_verified` is false. Identification is safe to merge; programming
is not, until the capture confirms the offsets.

### 3.2 Memory — medium, the real work

Linux, per `gmc_v11_0_mc_init()` (`gmc_v11_0.c:687`):

1. VRAM size comes from `nbio.funcs->get_memsize()` = `RCC_DEV0_EPF0_0_RCC_CONFIG_MEMSIZE` — the same
   register the APU path already reads, so size discovery is not new.
2. A dGPU additionally calls `amdgpu_device_resize_fb_bar()` to try to grow BAR0 to the VRAM size (ReBAR).
3. `aper_base = pci_resource_start(BAR0)`, `visible_vram_size = min(aper_size, real_vram_size)`.
4. GTT gets its own 512 MiB window in GPU address space.

For nullarc this means three allocators instead of one:

- **VRAM pool** — real GDDR6, addressed by MC address (`vram_start + offset`), PTEs with the VRAM mtype,
  no SYSTEM bit. CPU access only inside the BAR0 window.
- **CPU-visible VRAM pool** — the part of VRAM inside the mapped window; this is what replaces today's
  "VRAM pool at `fPoolPhys`". Without ReBAR this is 256 MiB minus reserves; with ReBAR it is nearly all
  8 GB.
- **GTT pool** — system memory, PTEs exactly like today (`kPteSysFlags`). RADV already allocates here
  through `AMDGPU_GEM_DOMAIN_GTT`, so the existing path keeps working.

Stage 1 implements and tests the plan (`plan_memory_aperture()` with `is_apu = false`): visible window,
high pool, TMR reserve at the top, console carved out of the pool floor. Both a 256 MiB window and a full
ReBAR window are covered by tests.

Stage 2 turns the plan into allocators. The hard part is not the allocator but making GPUVM per-domain:
`vmMap()` has to know whether a BO lives in VRAM or GTT, and the PTE flags have to follow. The
`phxn_gem_create_in.domains` field already distinguishes `AMDGPU_GEM_DOMAIN_VRAM` from `_GTT`, so the ABI
does not need to change.

### 3.3 PSP and firmware — medium, mostly a different sequence

Differences that matter, all from `amdgpu_psp.c` / `psp_v13_0.c`:

- **TOC**: Phoenix (13.0.4) loads `psp_13_0_4_toc.bin` and lets the PSP compute the TMR size. PSP
  13.0.7/13.0.10 have no TOC payload at all — the driver loads SOS and TA directly and uses the fixed
  4 MiB `PSP_TMR_SIZE` (`amdgpu_psp.h:40`). The `bringup5.sh` sequence that stages a TOC file does not
  apply.
- **TMR**: `boot_time_tmr = false` for 13.0.0/13.0.7/13.0.10, so the TMR must be allocated in VRAM by
  the driver and announced in `SETUP_TMR` (`cmd_setup_tmr.buf_phy_addr_*` = TMR MC address,
  `system_phy_addr_*` = its VRAM physical address). The APU path instead reads the firmware's pre-placed
  TMR through C2PMSG 35/36.
- **RLC autoload** stays enabled for 13.0.7/13.0.10 (`autoload_supported` defaults true), so the
  LOAD_IP_FW list is still the right mechanism — with the dGPU's own file names
  (`gc_11_0_2_pfp/me/mec/mes_2/mes1/imu/rlc`, `sdma_6_0_2`).
- **SMU firmware**: on a dGPU the SMU firmware is loaded as part of the PSP flow (`psp_load_smu_fw()`),
  not through `EnableGfxImu` the way the APU path powers GFX on. `imu_v11_0_start()` only calls
  `amdgpu_dpm_set_gfx_power_up_by_imu()` when `AMD_IS_APU` — the dGPU IMU start is the plain
  `CRESET = 0` plus a reset-status wait.

`rdna3_fw_plan.cpp` is already built from the firmware headers, so it needs file names and ordering, not
a rewrite. `tools/phxctl/bringup5.sh` and `mesboot.c` hardcode Phoenix addresses (`VRAM_MC 0x8000000000`,
`FB_PA 0x6A0000000`, `GART_VA`, firmware offsets); stage 2 replaces those with values from the plan.

### 3.4 SMU / power — medium

Navi 33 uses `smu_v13_0_7_ppt.c` (`smu_v13_0_7_set_ppt_funcs`, mailbox 66/82/90 at MP1 BASE_IDX 0). The
APU path's `EnableGfxImu` message does not exist there. Bring-up needs the PMFW handshake
(`SetDriverDramAddr*`, `TransferTable*`, feature enable) before GFX can run at speed, but a first
bring-up can run at VBIOS defaults with the SMU only initialized.

### 3.5 Display — hard, and the reason to stage

Three cases, in increasing difficulty:

1. **dGPU is the boot VGA device.** The firmware GOP has already lit a display and left a surface in
   VRAM. `phxfb.cpp` already knows how to adopt a firmware console (`getConsoleInfo()`); the surface
   would simply be reached through BAR0 at its VRAM offset instead of a system carve-out. This is a
   realistic MVP.
2. **No display at all.** Compute/offscreen only. Still useful (Metal compute, benchmarks) and much
   cheaper than case 1 or 3.
3. **Full DCN 3.2.1 init with DP/HDMI link training.** This is where issue #4 ("external displays")
   already sits for the APU; dGPU output is not easier. Out of scope until 1 and 2 work.

### 3.6 Metal / RADV — moderate, mostly identity plumbing

Mesa's RADV already contains a canned `navi33` device (device id `0x7480`, GC 11.0.0 family, 8 GB,
128-bit, 32 CU, 8 RB, PCIe x8). nullarc's patches (0001–0007) make the canned device selectable, report
the marketing name and the topology from the kext, and route AMDGPU_INFO through the service. For a dGPU
that means:

- a second canned name (`navi33`) chosen when the kext reports a discrete identity,
- a topology answer with 32 CU / 8 RB and the real VRAM size instead of the captured 760M numbers,
- one `acd_core` per device rather than the single `g_core` — required only if an APU and a dGPU are
  both present.

The winsys contract (`src/winsys/radv_r3n_winsys.cpp`, `src/uapi/r3n_user_client.cpp`) is already
device-agnostic apart from one hardcoded fact: `radv_r3n_winsys.cpp:28` reports
`has_dedicated_vram = false` with the comment "APU". Stage 6 turns that into a value the kext supplies.

### 3.7 Hybrid iGPU + dGPU — later

Every layer above assumes one GPU: the service name, the single canned device, the single `NVMTLDriver`
bundle. A dGPU-only configuration (dGPU as the only matched device, iGPU left to the framebuffer
driver) avoids all of it and should be the target of the first working build.

---

## 4. Staged plan

Each stage is a separate pull request and must not change behaviour for the existing APU path.

| # | Scope | Acceptance | Hardware needed |
|---|---|---|---|
| **1** (this PR) | APU/dGPU classification; dedicated-VRAM plan; capture tool; this document | `make test` green, ASan/UBSan clean, no `IOPCIMatch` change, `chip_profile()` still refuses dGPU | none |
| **2** | Navi 3x `ChipProfile` rows from the capture; Navi 3x ids added to `IOPCIMatch` with a boot-arg gate; VRAM/GTT allocators and per-domain PTEs; BAR0 window sizing | A dGPU is matched when enabled, its IP table parsed, VRAM size read from `RCC_CONFIG_MEMSIZE`, BOs in VRAM reachable through BAR0; APU regression test unchanged | RX 7600 + macOS boot |
| **3** | PSP flow for 13.0.7/13.0.10: no TOC, driver-allocated TMR, SOS/TA, LOAD_IP_FW with `gc_11_0_2`/`sdma_6_0_2`, RLC autoload, IMU start | PSP answers, all firmware loaded, GFX engine answers a ring test | RX 7600 |
| **4** | SMU 13.0.7 bring-up at VBIOS defaults; clocks reported; no `EnableGfxImu` | GPU runs a compute submission at stable clocks | RX 7600 |
| **5** | Display: GOP-passthrough framebuffer when the dGPU is the boot GPU | Desktop composited on the card's own output | RX 7600 as boot GPU |
| **6** | RADV `navi33` identity + topology; per-device core; hybrid | `vulkaninfo` reports Navi 33 with real VRAM; a triangle renders | RX 7600 |
| 7 | DCN 3.2.1 modeset + DP/HDMI link training | native modes, hotplug | RX 7600 |

Ordering rationale: 2 and 3 are independent of display and unlock everything else; 5 can be attempted as
soon as 2 works if the card is the boot GPU; 6 needs 2 and 3; 7 is the largest single piece and is
deliberately last.

### Risks

- **No resource rebalancing on macOS.** BAR sizes are whatever the firmware assigned. If BAR0 stays at
  256 MiB the high pool has to be reachable through GPUVM, which stage 2 must prove; ReBAR cannot be
  assumed.
- **VBIOS assumptions.** Power/clock tables and the GOP surface location are board-specific. The capture
  includes the VBIOS for maintainers; it is not redistributable, so nothing board-specific may be
  committed.
- **Firmware licensing.** Same rule as the APU path: firmware comes from linux-firmware at install time,
  never from the repository.
- **Hybrid configurations** multiply the identity problem; explicitly out of scope until the dGPU-only
  path works.

### Non-goals

Navi 31/32 bring-up in the first hardware stage, external displays before stage 7, video decode (VCN
4.0.4), and Metal on a hybrid setup.

---

## 5. What stage 1 changes

| File | Change |
|---|---|
| `src/amd/rdna3_chip_profile.h/.cpp` | `ChipKind`, `ChipFamily`, `ChipIdentity`, `chip_identity()`, `chip_family_name()`, `chip_is_apu()`; `ChipProfile::kind` and `regs_verified`; `chip_regs()` refuses unverified rows; fixes a latent build break (`HwId::Vcn` does not exist) |
| `src/amd/rdna3_rebar_uma.h/.cpp` | `is_apu` is finally honoured: dedicated VRAM keeps the console out of the visible pool, adds `PoolFloorBeyondLimit`, documents both layouts |
| `tests/test_rdna3_dgpu.cpp` | synthetic Navi 33 table, family split for 11.0.0/2/3, unknown-GC refusal, Phoenix regression, 256 MiB and 8 GiB windows, console carve-out, map cap, ReBAR mismatch |
| `tools/linux-dgpu/` | read-only capture script and its README |
| `Makefile` | builds `test_rdna3_dgpu` |

No `Info.plist` change, no new boot-arg, no code path that a current 780M/760M/740M user can reach.

---

## 6. Hardware data contract

Stage 2 cannot be written from public sources alone: the IP tuple and the register offsets have to come
from the card. `tools/linux-dgpu/collect.sh` produces exactly that, read-only:

- `ip-discovery-*.bin` — every block's version and segment base (the table `ChipProfile` matches),
- `identity.txt`, `lspci-vvv.txt`, `bars.txt` — ids, BAR0 size, link width/speed,
- `dmesg-amdgpu.txt` — the order Linux brings the card up,
- `memory.txt`, `topology.json` — VRAM/GTT sizes and the DRM topology in the format the 760M profile
  already uses,
- `firmware-rdna3.txt` — which `gc_11_0_x` / `psp_13_0_x` / `smu_13_0_x` files this board actually needs.

The VBIOS is captured too but must not be committed (AMD/OEM copyright). See
`tools/linux-dgpu/README.md`.

---

## 7. Open questions for maintainers

1. Is a dGPU-only configuration (iGPU left to the firmware framebuffer) an acceptable first target, or
   should hybrid be designed in from the start?
2. Should the dGPU live in `PhxBringup` behind a boot-arg, or in a separate `NaviBringup` service so the
   two can coexist and the APU path stays untouched?
3. Is a `hardware/profiles/navi33.json` capture, in the same format as `hp-760m.json`, the preferred way
   to carry the board data (as opposed to a `ChipProfile` row only)?
4. For display, is the GOP-passthrough MVP acceptable as an intermediate, given that it inherits whatever
   mode the firmware chose?

---

## Appendix A — Linux reference index

| Topic | Source |
|---|---|
| APU vs dGPU. `AMD_IS_APU` for GC 11.0.1/11.0.4 only | `amdgpu_discovery.c`, `amdgpu_discovery_set_ip_blocks()` |
| VRAM size, BAR resize, apertures | `gmc_v11_0.c`, `gmc_v11_0_mc_init()` |
| VRAM size register | `nbio_v7_7.c`, `nbio_v7_7_get_memsize()` |
| MMHUB 3.0.1 vs 3.0.2 register offsets | `gmc_v11_0.c`, `gmc_v11_0_set_mmhub_funcs()`; `mmhub_3_0_2_offset.h` |
| PSP variants, TMR, TOC | `amdgpu_psp.c`, `psp_early_init()`, `psp_tmr_init()`, `psp_load_toc()` |
| PSP 13.0 without TOC | `psp_v13_0.c` (MODULE_FIRMWARE list) |
| SMU 13.0.7 | `smu_v13_0_7_ppt.c`, `smu_v13_0.c` |
| IMU start is APU-specific | `imu_v11_0.c`, `imu_v11_0_start()` |
| external_rev_id per GC version | `soc21.c`, `soc21_common_early_init()` |
| Canned Navi 33 device | Mesa `src/amd/common/amdgpu_devices.c` |
| gfx1100/1101/1102/1103 numbering | Mesa `src/amd/common/ac_gpu_info.c` |
