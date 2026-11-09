// phx_dmub_layout.h: the DMCUB firmware pieces and memory windows for dcn_3_1_4_dmcub.bin, computed as Linux computes
// them. Pure computation; the loader (tools/phxctl/dmcubload.c) writes what this plans.
//
// Sources (Linux v6.17):
//   drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.h   struct dmcub_firmware_header_v1_0 (common header + inst_const_bytes,
//                                               bss_data_bytes)
//   drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.c   amdgpu_ucode_init_single_fw, AMDGPU_UCODE_ID_DMCUB: the PSP payload is
//                                               inst_const_bytes from ucode_array_offset_bytes
//   drivers/gpu/drm/amd/amdgpu/amdgpu_psp.c     psp_get_fw_type: AMDGPU_UCODE_ID_DMCUB -> GFX_FW_TYPE_DMUB (51,
//                                               psp_gfx_if.h)
//   drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm.c  dm_dmub_sw_init (region params: inst_const_size =
//                                               inst_const_bytes - PSP_HEADER_BYTES - PSP_FOOTER_BYTES, fw_inst_const at
//                                               ucode offset + PSP_HEADER_BYTES, fw_bss_data after inst_const, vbios =
//                                               adev->bios; every window DMUB_WINDOW_MEMORY_TYPE_FB) and dm_dmub_hw_init
//   drivers/gpu/drm/amd/display/dmub/src/dmub_srv.c  dmub_srv_calc_region_info / dmub_srv_calc_regions_for_memory_type
//                                               (base aligned to 256, size aligned to 64, fb size aligned to 4096),
//                                               dmub_get_fw_meta_info, the window sizes and CW bases
//   drivers/gpu/drm/amd/display/dmub/inc/dmub_cmd.h  struct dmub_fw_meta_info, DMUB_FW_META_MAGIC/OFFSET, DMUB_RB_SIZE,
//                                               DMUB_FW_HEADER_SHARED_STATE_SIZE
#pragma once
#include <stdint.h>

namespace phxdmub {

static constexpr uint32_t kPspHeaderBytes = 0x100, kPspFooterBytes = 0x100;   // amdgpu_dm.c
static constexpr uint32_t kPspFwTypeDmub = 51;                                 // GFX_FW_TYPE_DMUB
static constexpr uint32_t kStackSize = 128 * 1024, kContextSize = 512 * 1024, kRbSize = 64 * 128,
                          kMailboxSize = 2 * kRbSize, kFwStateSize = 64 * 1024, kTraceBufferSize = 64 * 1024,
                          kScratchMemSize = 1024, kIbMemSize = 1280, kSharedStateSize = (3 + 1 + 1) * 256,
                          kLsdmaRbSize = 64 * 1024, kFbAlignment = 1024 * 1024;
static constexpr uint32_t kMetaMagic = 0x444D5542, kMetaOffset = 0x24;
static constexpr uint32_t kCw0Base = 0x60000000, kCw1Base = 0x61000000, kCw3Base = 0x63000000, kCw4Base = 0x64000000,
                          kCw5Base = 0x65000000, kCw6Base = 0x66000000, kRegion5Base = 0xA0000000;
static constexpr uint32_t kTraceBufferEntryOffset = 16;

enum Window : uint32_t { kInstConst, kStack, kBssData, kVbios, kMailbox, kTraceBuff, kFwState, kScratchMem, kIbMem,
                         kSharedState, kLsdmaBuffer, kWindowCount };

struct Region { uint32_t base, top; };   // byte offsets inside the DMUB buffer (dmub_region)

struct Layout {
    // the firmware file
    uint32_t ucodeOffset;      // header.ucode_array_offset_bytes
    uint32_t instConstBytes;   // header inst_const_bytes (PSP payload size, PSP header/footer included)
    uint32_t bssDataBytes;     // header bss_data_bytes
    uint32_t pspOffset, pspSize;           // the PSP payload in the file (amdgpu_ucode_init_single_fw)
    uint32_t instConstOffset, instConstSize;   // the code proper (region params)
    uint32_t bssOffset;        // bss/data in the file
    // metadata (dmub_fw_meta_info), 0 when the firmware has none
    bool hasMeta;
    uint32_t fwVersion, metaFwStateSize, metaTraceSize, metaSharedSize;
    uint8_t metaDalFw;
    // windows
    Region regions[kWindowCount];
    uint32_t fbSize;           // aligned to 4096
};

static inline uint32_t align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
static inline uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// dmub_get_fw_meta_info_from_blob: the meta union (64 bytes: dmub_fw_meta) ends meta_offset bytes before the end
static inline const uint8_t *metaFromBlob(const uint8_t *blob, uint32_t size, uint32_t metaOffset) {
    const uint32_t kMetaUnion = 64;
    if (!blob || !size || size < kMetaUnion + metaOffset) return nullptr;
    const uint8_t *m = blob + size - metaOffset - kMetaUnion;
    return le32(m) == kMetaMagic ? m : nullptr;
}

// false: not a dmcub_firmware_header_v1_0 file, or its pieces do not fit in it
inline bool plan(const uint8_t *fw, uint32_t fwSize, uint32_t vbiosSize, Layout *L) {
    if (fwSize < 40) return false;
    *L = Layout{};
    L->ucodeOffset = le32(fw + 24);       // common_firmware_header.ucode_array_offset_bytes
    L->instConstBytes = le32(fw + 32);    // dmcub_firmware_header_v1_0.inst_const_bytes
    L->bssDataBytes = le32(fw + 36);      // .bss_data_bytes
    if (L->instConstBytes < kPspHeaderBytes + kPspFooterBytes) return false;
    if ((uint64_t)L->ucodeOffset + L->instConstBytes + L->bssDataBytes > fwSize) return false;
    L->pspOffset = L->ucodeOffset;
    L->pspSize = L->instConstBytes;
    L->instConstOffset = L->ucodeOffset + kPspHeaderBytes;
    L->instConstSize = L->instConstBytes - kPspHeaderBytes - kPspFooterBytes;
    L->bssOffset = L->ucodeOffset + L->instConstBytes;

    // dmub_get_fw_meta_info: legacy region at the end of bss/data, else the end of inst_const (16-byte aligned search)
    const uint8_t *meta = nullptr;
    if (L->bssDataBytes) meta = metaFromBlob(fw + L->bssOffset, L->bssDataBytes, kMetaOffset);
    else
        for (uint32_t i = 0; i < 16 && !meta; i++) meta = metaFromBlob(fw + L->instConstOffset, L->instConstSize, i);
    uint32_t fwState = kFwStateSize, trace = kTraceBufferSize, shared = kSharedStateSize;
    if (meta) {
        L->hasMeta = true;
        L->metaFwStateSize = fwState = le32(meta + 4);
        L->metaTraceSize = trace = le32(meta + 8);
        L->fwVersion = le32(meta + 12);
        L->metaDalFw = meta[16];
        L->metaSharedSize = shared = le32(meta + 20);
    }
    uint32_t sizes[kWindowCount] = {};
    sizes[kInstConst] = L->instConstSize;
    sizes[kStack] = kStackSize + kContextSize;
    sizes[kBssData] = L->bssDataBytes;
    sizes[kVbios] = vbiosSize;
    sizes[kMailbox] = kMailboxSize;
    sizes[kTraceBuff] = trace;
    sizes[kFwState] = fwState;
    sizes[kScratchMem] = kScratchMemSize;
    sizes[kIbMem] = kIbMemSize;
    sizes[kSharedState] = shared > kSharedStateSize ? shared : kSharedStateSize;
    sizes[kLsdmaBuffer] = kLsdmaRbSize;
    uint32_t top = 0;   // every window is DMUB_WINDOW_MEMORY_TYPE_FB (dm_dmub_sw_init)
    for (uint32_t i = 0; i < kWindowCount; i++) {
        L->regions[i].base = align(top, 256);
        L->regions[i].top = L->regions[i].base + align(sizes[i], 64);
        top = L->regions[i].top;
    }
    L->fbSize = align(top, 4096);
    return true;
}

// The DMCUB windows as dmub_srv_hw_init sets them up, for a DMUB buffer at MC address `mc`. offset = the window's GPU
// (MC) address; base/top = the address DMCUB sees.
struct Cw { uint64_t offset; uint32_t base, top; };
struct Windows { Cw cw3, cw4, cw5, cw6; uint32_t region5Top; uint32_t inbox1Base, inbox1Size; uint32_t cw2Base; };

inline Windows windows(const Layout &L, uint64_t mc) {
    Windows w{};
    const Region *r = L.regions;
    const uint32_t instSize = r[kInstConst].top - r[kInstConst].base;
    w.cw2Base = kCw0Base + instSize;   // cw2.region.base (not programmed on DCN 3.1: dmub_dcn31_setup_windows)
    w.cw3 = { mc + r[kVbios].base, kCw3Base, kCw3Base + (r[kVbios].top - r[kVbios].base) };
    w.cw4 = { mc + r[kMailbox].base, kCw4Base, kCw4Base + (r[kMailbox].top - r[kMailbox].base) };
    w.cw5 = { mc + r[kTraceBuff].base, kCw5Base, kCw5Base + (r[kTraceBuff].top - r[kTraceBuff].base) };
    w.cw6 = { mc + r[kFwState].base, kCw6Base, kCw6Base + (r[kFwState].top - r[kFwState].base) };
    w.region5Top = w.cw5.top - w.cw5.base - 1;   // dmub_dcn31_setup_windows
    w.inbox1Base = w.cw4.base;                   // inbox1 = first half of the mailbox
    w.inbox1Size = kRbSize;
    return w;
}

} // namespace phxdmub
