//
//  rdna3_fw_plan.h — Which firmware payloads the PSP loads (LOAD_IP_FW) for GFX bring-up, in which order and where in
//  fw_buf each is staged, built from the firmware files' own headers for the chip's IP versions. Freestanding C++17.
//
//  Replaces the byte offsets tools/phxctl/bringup5.sh hardcoded for gc_11_0_1 (they do not fit gc_11_0_4: its
//  mes1.bin ucode is 103600 B, not 104272). For Phoenix the plan reproduces that script's list exactly
//  (tests/test_rdna3_fw_plan.cpp).
//
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "rdna3_ucode_extract.h"

namespace rdna3 {

// The files, in load order.
enum class FwFile : uint8_t { CpPfp, CpMe, CpMec, Sdma, Mes, MesKiq, Imu, Rlc, Count };
constexpr size_t kFwFileCount = static_cast<size_t>(FwFile::Count);

struct FwLoad {
    FwFile file;
    uint32_t offset, size;   // the payload inside the file (from its header)
    uint32_t fw_type;        // PSP GFX_FW_TYPE_* (psp_gfx_if.h)
    uint32_t dst;            // byte offset in fw_buf where it is staged before LOAD_IP_FW
};
constexpr uint32_t kMaxFwLoads = 32;

// "<prefix><suffix>" for a file: gc_prefix for GC files (e.g. "gc_11_0_4"), sdma_prefix for SDMA ("sdma_6_0_1").
bool fw_file_name(FwFile f, const char *gc_prefix, const char *sdma_prefix, char *out, size_t out_len);

// The LOAD_IP_FW list for these files (bins/sizes indexed by FwFile). Returns the count, or 0 if a file is missing or
// malformed, a payload type has no fw_buf slot, or a payload does not fit its slot: then nothing must be loaded.
uint32_t fw_plan(const uint8_t *const bins[kFwFileCount], const uint64_t sizes[kFwFileCount], FwLoad out[kMaxFwLoads]);

// The TOC payload of "<psp>_toc.bin" (amdgpu_psp.c psp_init_toc_microcode: psp_firmware_header_v1_0, size =
// header.ucode_size_bytes at header.ucode_array_offset_bytes). False if the file is malformed.
bool toc_payload(const uint8_t *bin, uint64_t size, uint32_t &offset, uint32_t &len);

} // namespace rdna3
