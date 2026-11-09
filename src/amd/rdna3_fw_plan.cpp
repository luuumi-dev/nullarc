//
//  rdna3_fw_plan.cpp — LOAD_IP_FW plan from the firmware headers.
//
#include "rdna3_fw_plan.h"

namespace rdna3 {

namespace {

struct FileSpec {
    const char *suffix;
    bool sdma;           // prefixed with the SDMA prefix rather than the GC one
    uint64_t host_type;  // rdna3_ucode_extract file kind
};

// Names: gfx_v11_0.c gfx_v11_0_init_microcode ("%s_pfp.bin", "%s_me.bin", "%s_mec.bin", "%s_rlc.bin"),
// amdgpu_sdma.c amdgpu_sdma_init_microcode ("%s.bin" for instance 0), amdgpu_mes.c amdgpu_mes_init_microcode (GC 11.x:
// "%s_mes_2.bin" scheduler pipe, "%s_mes1.bin" KIQ pipe), imu_v11_0.c imu_v11_0_init_microcode ("%s_imu.bin").
// Order: upstream bringup5.sh, which follows the PSP command order of the Linux register trace on the 780M; RLC_G is the
// last payload (rdna3_ucode_extract: loading it starts the RLC autoload).
constexpr FileSpec kFiles[kFwFileCount] = {
    {"_pfp.bin", false, kHostFwFile_CP_PFP},
    {"_me.bin", false, kHostFwFile_CP_ME},
    {"_mec.bin", false, kHostFwFile_CP_MEC},
    {".bin", true, kHostFwFile_SDMA},
    {"_mes_2.bin", false, kHostFwFile_MES},
    {"_mes1.bin", false, kHostFwFile_MES_KIQ},
    {"_imu.bin", false, kHostFwFile_IMU},
    {"_rlc.bin", false, kHostFwFile_RLC},
};

// fw_buf slot of each payload type: upstream bringup5.sh's offsets (unchanged so the 780M stages exactly as before),
// with the room up to the next slot as the cap.
struct Slot {
    uint32_t fw_type, dst, cap;
};
constexpr Slot kSlots[] = {
    {2, 0x000000, 0x100000},   // CP_PFP
    {1, 0x100000, 0x100000},   // CP_ME
    {4, 0x200000, 0x100000},   // CP_MEC
    {8, 0x300000, 0x080000},   // RLC_G
    {71, 0x380000, 0x040000},  // SDMA_UCODE_TH0
    {72, 0x3C0000, 0x040000},  // SDMA_UCODE_TH1
    {33, 0x400000, 0x040000},  // CP_MES
    {34, 0x440000, 0x040000},  // MES_STACK
    {81, 0x480000, 0x040000},  // CP_MES_KIQ
    {82, 0x4C0000, 0x040000},  // MES_KIQ_STACK
    {68, 0x500000, 0x100000},  // IMU_I
    {69, 0x600000, 0x100000},  // IMU_D
    {20, 0x700000, 0x020000},  // RLC_RESTORE_LIST_GPM_MEM
    {21, 0x720000, 0x0E0000},  // RLC_RESTORE_LIST_SRM_MEM
    {26, 0x800000, 0x040000},  // RLC_IRAM
    {48, 0x840000, 0x040000},  // RLC_DRAM_BOOT
    {25, 0x880000, 0x040000},  // RLC_P
};

const Slot *slot_for(uint32_t fw_type) {
    for (const Slot &s : kSlots)
        if (s.fw_type == fw_type)
            return &s;
    return nullptr;
}

} // namespace

bool fw_file_name(FwFile f, const char *gc_prefix, const char *sdma_prefix, char *out, size_t out_len) {
    const size_t i = static_cast<size_t>(f);
    if (i >= kFwFileCount || !gc_prefix || !sdma_prefix || !out)
        return false;
    size_t n = 0;
    for (const char *c = kFiles[i].sdma ? sdma_prefix : gc_prefix; *c; c++) {
        if (n + 1 >= out_len) return false;
        out[n++] = *c;
    }
    for (const char *c = kFiles[i].suffix; *c; c++) {
        if (n + 1 >= out_len) return false;
        out[n++] = *c;
    }
    out[n] = '\0';
    return true;
}

uint32_t fw_plan(const uint8_t *const bins[kFwFileCount], const uint64_t sizes[kFwFileCount], FwLoad out[kMaxFwLoads]) {
    uint32_t n = 0;
    for (size_t f = 0; f < kFwFileCount; f++) {
        if (!bins[f] || !sizes[f])
            return 0;
        UcodePayload p[kMaxUcodePayloadsPerFile];
        const uint32_t k = rdna3_ucode_extract(kFiles[f].host_type, bins[f], sizes[f], p);
        if (k == 0)
            return 0;
        for (uint32_t j = 0; j < k; j++) {
            const Slot *s = slot_for(p[j].fw_type);
            if (!s || p[j].size_bytes > s->cap || n >= kMaxFwLoads)
                return 0;
            out[n++] = {static_cast<FwFile>(f), p[j].offset_bytes, p[j].size_bytes, p[j].fw_type, s->dst};
        }
    }
    return n;
}

bool toc_payload(const uint8_t *bin, uint64_t size, uint32_t &offset, uint32_t &len) {
    if (!bin || size < sizeof(common_firmware_header))
        return false;
    const auto *h = reinterpret_cast<const common_firmware_header *>(bin);
    const uint64_t off = h->ucode_array_offset_bytes, sz = h->ucode_size_bytes;
    if (h->header_version_major != 1 || sz == 0 || off + sz > size || off + sz < off)
        return false;
    offset = static_cast<uint32_t>(off);
    len = static_cast<uint32_t>(sz);
    return true;
}

} // namespace rdna3
