/*
 * PhxNativeABI.h — the "N48N" user-client ABI (v1, selectors 0..8) as spoken by the Cobalt64 RADV Darwin
 * backend, for the Phoenix (Radeon 780M) bring-up kext.
 *
 * Layouts and selector numbers follow Cobalt64 src/navi48-bringup/src/Navi48NativeABI.h
 * (BSD-3-Clause, (c) 2026 the Navi48 project); the AMDGPU_* values are those of Linux/Mesa
 * include/drm-uapi/amdgpu_drm.h (MIT). Only what selectors 0..8 need is here.
 *
 * Transport: IOServiceOpen(PhxBringup, task, PHXN_UC_TYPE), IOConnectCallMethod(selector, ...).
 * Scalar counts and struct sizes are checked exactly. CPU map of a BO: IOConnectMapMemory64(memoryType = handle).
 */
#ifndef PHX_NATIVE_ABI_H
#define PHX_NATIVE_ABI_H

#include <stdint.h>

#define PHXN_UC_TYPE      0x4E34384Eu   /* 'N48N' */
#define PHXN_ABI_VERSION  1u
#define PHXN_ABI_MINOR    12u           /* minor the RADV build expects in Hello / info.reserved[2] */
#define PHXN_HELLO_F_MINOR (1u << 8)
#define PHXN_MAX_IBS      64u
#define PHXN_MAX_BOS      4096u         /* handles 1..4095 */
#define PHXN_MAX_CTX      64u           /* ids 1..64 */
#define PHXN_FENCE_SLOTS  16u
#define PHXN_WAIT_CAP_NS  2000000000ull
#define PHXN_SEQ_LAST     0xFFFFFFFFFFFFFFFFull
#define PHXN_VMID         8u

enum {
    PHXN_SEL_HELLO     = 0,
    PHXN_SEL_QUERYINFO = 1,
    PHXN_SEL_READREGS  = 2,
    PHXN_SEL_BOCREATE  = 3,
    PHXN_SEL_BOFREE    = 4,
    PHXN_SEL_GEMVA     = 5,
    PHXN_SEL_CTX       = 6,
    PHXN_SEL_SUBMIT    = 7,
    PHXN_SEL_WAITSEQ   = 8,
    PHXN_SEL_BOIMPORTHOST = 21,    /* ABI 1.9: in [0] host VA [1] size [2] flags 0 [3] GPU VA 0 -> out [0] handle [1] size [2] 0 [3] bits */
    /* Phoenix-only debug selectors: root, and only with the boot-arg phxdebug=1. */
    PHXN_SEL_DBG_SMU   = 100,  /* in: [0] SMU 13.0.4 message id (allow-list) [1] argument   out: [0] response [1] argument register after */
    PHXN_SEL_DBG_PSP_RING = 101, /* in: [0] op (1 create KM ring, 2 destroy, 3 status) [1] VRAM byte offset (create)
                                    out: [0] C2PMSG_64 after [1] ring MC address [2] region was zero (create) [3] C2PMSG_67 (wptr) */
    PHXN_SEL_DBG_VRAM_READ = 102, /* in: [0] VRAM byte offset (dword-aligned) [1] dword count (1..256); struct out = count dwords,
                                     read through the BAR0 framebuffer aperture (read-only) */
    PHXN_SEL_DBG_PSP_SUBMIT = 103, /* in: [0] GPCOM cmd id (allow-list: FB_FW_RESERV_ADDR/EXT only)
                                      out: [0] resp.status [1] fence us [2] uresp dword0 [3] uresp dword1 */
    PHXN_SEL_DBG_FW_STAGE = 104,   /* in: [0] fw_buf byte offset (dword-aligned); struct in = bytes (<=4096).
                                      Copies the chunk into the VRAM fw_buf through BAR0 (no submit). */
    PHXN_SEL_DBG_PSP_LOADIPFW = 105, /* in: [0] fw_type [1] fw_buf byte offset [2] size; submits GFX_CMD_ID_LOAD_IP_FW
                                        out: [0] resp.status [1] fence us [2] resp fw_addr_lo [3] resp fw_addr_hi */
    PHXN_SEL_DBG_PSP_CTL = 106,    /* in: [0] op (1 SETUP_TMR, 2 AUTOLOAD_RLC) [1] arg (TMR size for op 1)
                                      out: [0] resp.status [1] fence us [2] resp dword@+8 [3] resp dword@+12 */
    PHXN_SEL_DBG_PSP_RAW = 107,    /* in: [0] cmd_id [1..6] six command-union dwords (at +28); submits it verbatim.
                                      out: [0] resp.status [1] fence us [2] resp dword@+8 [3] resp dword@+12
                                      For experimenting with SETUP_TMR / LOAD_TOC params. Addresses are built in
                                      userspace from phx-fb-mc-base and phx-fb-phys. */
    PHXN_SEL_DBG_WRITE_REG = 108,  /* in: [0] absolute dword register [1] value; writes MMIO. out[0] = readback.
                                      Ring/engine bring-up experimentation (root + phxdebug). */
    PHXN_SEL_DBG_VRAM_WRITE = 109, /* in: [0] VRAM byte offset (dword-aligned) [1] dword value; writes via BAR0. */
    PHXN_SEL_DBG_ADOPTQ = 112,     /* in: [0] ring VRAM off [1] ring dwords [2] wb VRAM off (wptr shadow at +16) [3] doorbell byte off [4] wptr [5] fence VRAM off | queue (0 compute, 1 gfx) */
    PHXN_SEL_DBG_CLAIM = 113,      /* in: [0] 1 = mark the GPU driven (nmxgpu-claimed) and re-run matching so PhxAccel starts. */
    PHXN_SEL_DBG_SYSMEM = 111,     /* in: [0] op (1 alloc [1] bytes, 2 read [1] off, 3 write [1] off [2] val, 4 free)  out: [0] phys  [1] len/value */
    PHXN_SEL_DBG_DOORBELL = 110,   /* in: [0] doorbell BAR2 byte offset (8-aligned) [1] value lo [2] value hi; writes a 64-bit doorbell. */
};

/* GPU memory layout registers (MMHUB 3.0.1 segment 1 = 0x1A000): Linux mmhub_v3_0_1_get_fb_location / get_mc_fb_offset */
#define PHX_MMMC_VM_FB_LOCATION_BASE 0x1A8ECu
#define PHX_MMMC_VM_FB_LOCATION_TOP  0x1A8EDu
#define PHX_MMMC_VM_FB_OFFSET        0x1A8D7u

/* PSP 13.0.4 mailbox (MP0 segment 1 = 0x16000), Linux psp_v13_0.c */
#define PHX_PSP_C2PMSG_58 0x1607Au  /* SOS version */
#define PHX_PSP_C2PMSG_64 0x16080u  /* ring control command / status */
#define PHX_PSP_C2PMSG_67 0x16083u  /* KM ring write pointer */
#define PHX_PSP_C2PMSG_69 0x16085u  /* ring MC address low */
#define PHX_PSP_C2PMSG_70 0x16086u  /* ring MC address high */
#define PHX_PSP_C2PMSG_71 0x16087u  /* ring size */
#define PHX_PSP_C2PMSG_81 0x16091u  /* SOS sign of life */
#define PHX_PSP_RING_SIZE 0x1000u      /* value written to C2PMSG_71 (kPSPKMRingSize) */
#define PHX_PSP_RING_BUF_SIZE 0x4000u  /* bytes of VRAM zeroed for the ring (kPSPKMRingBufSize, 16 KiB) */
#define PHX_PSP_RING_TYPE_KM 2u

/* GPCOM command submission (Linux psp_cmd_submit_buf / psp_gfx_if.h, Cobalt64 psp_v14_0.cpp). The ring frame is
 * 64 B; the command buffer is 1 KiB; the PSP writes psp_gfx_resp back into the same cmd buffer at +864. */
#define PHX_PSP_GFX_FRAME_SIZE 64u
#define PHX_PSP_CMD_BUF_SIZE   0x400u  /* kPSPGfxCmdRespSize = 1024 */
#define PHX_PSP_GFX_RESP_OFFSET 864u   /* psp_gfx_resp within the cmd buffer; status at +0, uresp at +64 */
#define PHX_PSP_GFX_URESP_OFFSET (PHX_PSP_GFX_RESP_OFFSET + 64u)
#define PHX_PSP_CMD_HDR_CMD_ID_OFFSET 8u /* cmd_id dword within psp_gfx_cmd_resp header */
#define PHX_PSP_CMD_REL   0x4000u      /* cmd buffer VRAM offset relative to the ring (ring + 16 KiB) */
#define PHX_PSP_FENCE_REL 0x8000u      /* fence VRAM offset relative to the ring (ring + 32 KiB) */
/* Safe no-payload query commands (do not change GPU power state) used to validate the ring end to end. */
#define PHX_PSP_GFX_CMD_FB_FW_RESERV_ADDR     0x50u
#define PHX_PSP_GFX_CMD_FB_FW_RESERV_EXT_ADDR 0x51u
#define PHX_PSP_ERR_UNKNOWN_COMMAND 0x100u

/* GPCOM command ids (psp_gfx_if.h) and the command union offset within the 1 KiB cmd buffer. */
#define PHX_PSP_GFX_CMD_UNION_OFFSET 28u
#define PHX_PSP_GFX_CMD_SETUP_TMR    5u
#define PHX_PSP_GFX_CMD_LOAD_IP_FW   6u
#define PHX_PSP_GFX_CMD_AUTOLOAD_RLC 0x21u

/* Fixed VRAM regions for firmware bring-up (absolute byte offsets, all within the 256 MiB BAR0 aperture, clear
 * of the console framebuffer [0,0xFA0000) and the ring/cmd/fence region near 17 MiB).
 * TMR (Trusted Memory Region) the PSP copies firmware into; fw_buf where the CPU stages ucode for LOAD_IP_FW. */
#define PHX_VRAM_TMR_OFF    0x1400000ull   /* 20 MiB */
#define PHX_VRAM_TMR_CAP    0x1000000ull   /* up to 16 MiB */
#define PHX_VRAM_FWBUF_OFF  0x4000000ull   /* 64 MiB */
#define PHX_VRAM_FWBUF_SIZE 0x1000000ull   /* 16 MiB */

/* HDP flush (NBIF 7.7.0): NBIO base[2]=0xd20 + regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL(0x12d) = dword 0xE4D; its value
 * is the BAR5 byte offset of the flush window (0x385c on this card). Writing 0 there drains the host data path. */
#define PHX_NBIO_REMAP_HDP_MEM_FLUSH_CNTL 0xE4Du
#define PHX_GFX_CTRL_CMD_ID_DESTROY_RINGS 0x00030000u
#define PHX_MBOX_TOS_MASK  0x8000FFFFu  /* response flag | status */
#define PHX_MBOX_TOS_READY 0x80000000u  /* response flag set, status 0 */

/* SMU 13.0.4 mailbox (Linux smu_v13_0_4_ppt.c: MP1_SMN_C2PMSG_66/82/90, MP1 segment 1 = 0x16000 here) */
#define PHX_SMU_MSG_REG  0x16282u
#define PHX_SMU_ARG_REG  0x16292u
#define PHX_SMU_RESP_REG 0x1629Au
/* PPSMC message ids (smu_v13_0_4_ppsmc.h) the debug selector accepts */
#define PHX_PPSMC_TestMessage          0x01u
#define PHX_PPSMC_GetPmfwVersion       0x02u
#define PHX_PPSMC_GetDriverIfVersion   0x03u
#define PHX_PPSMC_GetEnabledSmuFeatures 0x12u
#define PHX_PPSMC_AllowGfxOff          0x19u
#define PHX_PPSMC_DisallowGfxOff       0x1Au
#define PHX_PPSMC_EnableGfxImu         0x16u  /* smu_v13_0_4_ppsmc.h: power up GFX via the IMU (APU GFX bring-up) */
/* clocks (smu_v13_0_4_ppsmc.h; frequencies in MHz, as Linux's set_soft_freq_limited_range sends them) */
#define PHX_PPSMC_SetHardMinSocclkByFreq 0x13u
#define PHX_PPSMC_GetGfxclkFrequency   0x17u
#define PHX_PPSMC_GetFclkFrequency     0x18u
#define PHX_PPSMC_SetSoftMaxGfxClk     0x1Bu
#define PHX_PPSMC_SetHardMinGfxClk     0x1Cu
#define PHX_PPSMC_SetSoftMaxSocclkByFreq 0x1Du
#define PHX_PPSMC_SetSoftMaxFclkByFreq 0x1Eu
#define PHX_PPSMC_SetHardMinFclkByFreq 0x23u

/* n48n_info.flags */
#define PHXN_INFO_HUNG         (1u << 0)
#define PHXN_INFO_S1B_POSITIVE (1u << 1)
#define PHXN_INFO_NATIVE_BOOT  (1u << 2)

/* BoCreate scalar out [3] */
#define PHXN_PLACED_CPU_MAPPABLE (1u << 0)
#define PHXN_PLACED_ZEROED       (1u << 1)
#define PHXN_PLACED_HI_POOL      (1u << 2)

#define PHXN_CS_HAS_FENCE (1u << 0)

/* amdgpu_drm.h values */
#define PHX_AMDGPU_GEM_DOMAIN_GTT  0x2u
#define PHX_AMDGPU_GEM_DOMAIN_VRAM 0x4u
#define PHX_AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED (1u << 0)
#define PHX_AMDGPU_GEM_CREATE_NO_CPU_ACCESS       (1u << 1)
#define PHX_AMDGPU_GEM_CREATE_CP_MQD_GFX9         (1u << 8)
#define PHX_AMDGPU_GEM_CREATE_ENCRYPTED           (1u << 10)
#define PHX_AMDGPU_GEM_CREATE_PREEMPTIBLE         (1u << 11)
#define PHX_AMDGPU_VA_OP_MAP     1u
#define PHX_AMDGPU_VA_OP_UNMAP   2u
#define PHX_AMDGPU_VA_OP_CLEAR   3u
#define PHX_AMDGPU_VA_OP_REPLACE 4u
#define PHX_AMDGPU_VM_PAGE_EXECUTABLE (1u << 3)
#define PHX_AMDGPU_VM_PAGE_PRT        (1u << 4)
#define PHX_AMDGPU_CTX_OP_ALLOC_CTX    1u
#define PHX_AMDGPU_CTX_OP_FREE_CTX     2u
#define PHX_AMDGPU_CTX_OP_QUERY_STATE2 4u
#define PHX_AMDGPU_HW_IP_GFX 0u
#define PHX_AMDGPU_HW_IP_COMPUTE 1u
#define PHX_AMDGPU_IB_FLAG_CE                     (1u << 0)
#define PHX_AMDGPU_IB_FLAG_PREAMBLE               (1u << 1)
#define PHX_AMDGPU_IB_FLAG_PREEMPT                (1u << 2)
#define PHX_AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE   (1u << 3)
#define PHX_AMDGPU_IB_FLAG_RESET_GDS_MAX_WAVE_ID  (1u << 4)
#define PHX_AMDGPU_IB_FLAGS_SECURE                (1u << 5)
#define PHX_AMDGPU_IB_FLAG_EMIT_MEM_SYNC          (1u << 6)

struct phxn_gem_create_in { uint64_t bo_size, alignment, domains, domain_flags; };

struct phxn_gem_va {
    uint32_t handle, _pad, operation, flags;
    uint64_t va_address, offset_in_bo, map_size;
    uint64_t vm_timeline_point;
    uint32_t vm_timeline_syncobj_out, num_syncobj_handles;
    uint64_t input_fence_syncobj_handles;
};

struct phxn_ctx { uint32_t op, flags, ctx_id; int32_t priority; };

struct phxn_cs_ib { uint32_t _pad, flags; uint64_t va_start; uint32_t ib_bytes, ip_type, ip_instance, ring; };

struct phxn_cs_in {
    uint32_t abi, ctx_id, num_ibs, flags;
    uint32_t fence_handle, fence_offset;
    uint64_t reserved;
};

struct phxn_info {
    uint32_t abi_version, kext_build, flags, vmid;
    uint32_t gb_addr_config, gc_version, ring_size_dw, max_ibs;
    uint64_t vram_vis_total, vram_vis_free;
    uint64_t vram_hi_total, vram_hi_free;
    uint64_t gtt_cap, gtt_used, gtt_max_bo;
    uint64_t pt_cap, pt_used;
    uint64_t seq_emitted, seq_retired;
    uint64_t va_low_first, va_low_last;
    uint64_t va_high_first, va_high_last;
    uint32_t max_bos, fence_slots;
    uint32_t reserved[8];
};

static_assert(sizeof(struct phxn_gem_create_in) == 32, "gem_create_in is 32 B");
static_assert(sizeof(struct phxn_gem_va) == 64, "gem_va is 64 B");
static_assert(sizeof(struct phxn_ctx) == 16, "ctx is 16 B");
static_assert(sizeof(struct phxn_cs_ib) == 32, "cs_ib is 32 B");
static_assert(sizeof(struct phxn_cs_in) == 32, "cs_in is 32 B");
static_assert(sizeof(struct phxn_info) == 192, "info is 192 B");

#endif /* PHX_NATIVE_ABI_H */
