# Discrete RDNA 3 capture (`tools/linux-dgpu`)

Read-only hardware capture for a discrete RDNA 3 card (Navi 31 / Navi 32 / Navi 33), used to fill the
`docs/dgpu-support.md` tables and to confirm the register map before any kext writes to a dGPU.

The reference card is an **RX 7600 (Navi 33, `1002:7480`, GC 11.0.2 / gfx1102)**, the dGPU issue #23 asks about.

## Run it

On a Linux system where `amdgpu` is loaded (any distro; a live USB is fine):

```sh
sudo ./collect.sh                 # picks the AMD display device that is not an APU
sudo ./collect.sh --skip-vbios    # when /rom cannot be enabled
sudo ./collect.sh 0000:03:00.0    # explicit BDF on a hybrid machine
```

Output goes to `dgpu-capture-<bdf>-<timestamp>/`. Nothing is written to the GPU, the driver or the
system; the only writes are the capture files and the one-byte enable/disable of
`/sys/bus/pci/devices/<bdf>/rom` around the VBIOS read.

Dependencies: `bash`, `coreutils`, `pciutils` (`lspci`), `kmod` (`modinfo`); optional `python3`,
`libdrm` headers and a C compiler for the topology JSON (`tools/linux-profile/collect.c`).

## What it collects

| File | Why nullarc needs it |
|---|---|
| `identity.txt` | vendor/device/subsystem/revision, driver and kernel: the `IOPCIMatch` key and the profile lookup key |
| `lspci-vvv.txt`, `bars.txt` | BAR0 size (256 MiB vs a ReBAR window), BAR2 doorbells, BAR5 MMIO, link speed/width |
| `ip-discovery-*.bin`, `ip-discovery-*.txt` | the **IP version of every block** (GC, MP0/PSP, MP1/SMU, MMHUB, NBIF, DMU, SDMA, VCN). This is the table `chip_identity()` classifies and the one `ChipProfile` must match exactly |
| `dmesg-amdgpu.txt` | the order Linux brings the card up: PSP SOS/TA, TMR, SMU, RLC autoload, MES, DCN, link training |
| `amdgpu-module.txt` | firmware search paths and the module's own firmware list |
| `firmware-rdna3.txt`, `.sha256` | which `gc_11_0_x` / `psp_13_0_x` / `smu_13_0_x` / `sdma_6_0_x` / `dcn_3_2_x` files the card actually needs |
| `memory.txt` | `mem_info_vram_total`, GTT size, visible VRAM: the numbers the memory plan is validated against |
| `topology.json`, `ip-blocks.txt` | the same `AMDGPU_INFO` capture format the 760M profile uses (`tools/linux-profile/collect.c`) |
| `vbios.rom`, `VFCT.bin` | board layout, power tables, GOP framebuffer location. **Do not commit these** |

## What to attach to a pull request

Safe to attach: `identity.txt`, `lspci-vvv.txt`, `bars.txt`, `dmesg-amdgpu.txt`,
`amdgpu-module.txt`, `memory.txt`, `ip-discovery*.bin`, `ip-discovery*.txt`, `topology.json`,
`firmware-rdna3.txt`, `firmware-rdna3.sha256`.

Do **not** attach `vbios.rom` or `VFCT.bin`: AMD/OEM copyright with no redistribution grant (the
repository `.gitignore` already excludes `*.rom`). A maintainer who needs the board data can ask for it
privately.

The capture contains no user data beyond the PCI address, subsystem ids and the firmware build the
distribution ships. Check `dmesg-full.txt` before sharing if the machine's hostname or disk paths are
sensitive; the per-file list above is enough for the analysis.

## Expected result on Navi 33

`ip-discovery-*.txt` should show, among others:

```
GC      11.0.2   (gfx1102)
MP0     13.0.7   (PSP, or 13.0.10 on newer VBIOS)
MP1     13.0.7   (SMU)
MMHUB    3.0.2
NBIF     7.7.0
DMU      3.2.1   (DCN 3.2.1)
SDMA0    6.0.2
VCN      4.0.4
```

`chip_identity()` maps GC 11.0.2 to `Navi33` / `ChipKind::Discrete` with no PCI id involved. If the
table disagrees with the list above, the capture wins: update `tests/test_rdna3_dgpu.cpp` and the
`docs/dgpu-support.md` reference table in the same commit.

## Interpretation notes

- **BAR0 size** decides the memory model: 256 MiB (no ReBAR) means most of the 8 GiB VRAM is only
  reachable through GPUVM; a full window makes the whole VRAM CPU-visible (see `plan_memory_aperture()`
  in `src/amd/rdna3_rebar_uma.cpp`).
- **GOP framebuffer**: if the card is the boot GPU, `dmesg` reports the EFI framebuffer base, and
  `lspci-vvv.txt` shows BAR0's physical address. The offset of the console inside BAR0 is what the
  capture is used to derive — it must stay outside the allocator pool.
- **PSP version matters**: 13.0.7 / 13.0.10 load SOS and TA with no TOC (`psp_v13_0.c`), while Phoenix
  (13.0.4) uses a TOC. That difference is the first thing to get right in the firmware plan.
