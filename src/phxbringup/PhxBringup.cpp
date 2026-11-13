//
//  PhxBringup.cpp — Phoenix (Radeon 780M, 1002:1900) bring-up kext, step E1.
//
//  Publishes the service the RADV Darwin backend opens (class PhxBringup, user client type 'N48N')
//  and answers ABI v1 selectors 0..8 with the same rules as Mesa's in-process mock kext
//  (src/amd/common/darwin/ac_darwin_mock.c): buffers are kernel/user shared system memory,
//  GPU VA mappings are bookkeeping only, and Submit retires at once without touching the GPU.
//  E1 proves the IOKit transport end to end; E3/E4 put the GPU behind the same calls.
//
//  Boot-arg -phxbringupoff disables it. ReadRegs (MMIO read) is root-only; nothing else touches hardware.
//

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <kern/thread_call.h>
#include <kern/clock.h>

#include "PhxNativeABI.h"
#include "../amd/phx_smu_pm.h"

#define PHX_KEXT_BUILD 37u

// debug.phxbos_flush: how a client's BOs are made coherent around each submission and each completed wait.
// 2 = clflush every line (the bring-up way), 1 = clflushopt (the same lines, pipelined), 0 = none (the GPU maps system
// memory snooped, kPteSysFlags). With clflush, WindowServer spent ~73% of its CPU here and composited ~13 fps; with
// none, 41 fps and no artifacts (2026-10-20), hence the default.
static int gBosFlush = 0;
static volatile SInt64 gBosFlushBytes;
SYSCTL_INT(_debug, OID_AUTO, phxbos_flush, CTLFLAG_RW | CTLFLAG_LOCKED, &gBosFlush, 0,
           "BO cache maintenance per submit/wait: 0 none, 1 clflushopt, 2 clflush");
SYSCTL_QUAD(_debug, OID_AUTO, phxbos_flush_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&gBosFlushBytes,
            "bytes of BOs flushed from the CPU caches");
// debug.phx_hang_ms: a queue holding work whose fence has not moved for this long gets the waves of its oldest
// submission's VMID killed (Linux's gfx_v11_0_ring_soft_recovery), and every wave if it still stands half as long
// again; 0 turns it off. One hung app (Geekbench's OpenCL Feature Matching, 2026-10-26) otherwise froze WindowServer
// until its watchdog took the session down; a spinning wave killed this way by hand freed the queue in place.
static int gHangMs = 3000;
static volatile SInt32 gHangKills;
SYSCTL_INT(_debug, OID_AUTO, phx_hang_ms, CTLFLAG_RW | CTLFLAG_LOCKED, &gHangMs, 0,
           "ms without progress before a hung submission's waves are killed (0 off)");
SYSCTL_INT(_debug, OID_AUTO, phx_hang_kills, CTLFLAG_RD | CTLFLAG_LOCKED, (int *)&gHangKills, 0,
           "hung submissions whose waves were killed");
// debug.phx_pm_*: SMU 13.0.4 power management. Every phx_pm_ms the SMU copies its metrics table (SmuMetrics_t) into
// one VRAM page (Linux smu_cmn_update_table) and the values below are refreshed. phx_pm_mode picks the GFX clock policy:
// 0 = leave the PMFW defaults (telemetry only), 1 = auto (Linux AMD_DPM_FORCED_LEVEL_AUTO: HardMin = min, SoftMax = max,
// the PMFW scales inside), 2 = governor (the kext moves SoftMax with the GFX load, for PMFWs that do not scale on their
// own). phx_pm_gfx_max lowers the ceiling (MHz, 0 = the PMFW's DPM table). The floor is always the PMFW's own minimum:
// a HardMin above it keeps the PMFW from pulling GFXCLK down when the package reaches its PPT/TDC limits, and on battery
// the EC then cut the power (ACEMAGIC RX16, 2026-11-12: twice, HardMin 2600 under GPU load, ThrottlerStatus
// FPPT|SPPT|TDC_VDD, package peaks ~60 W; the same load with the PMFW's own choice ran through).
static int gPmMs = 250;
static int gPmMode = 0;
static int gPmGfxMax = 0;
static int gPmUp = 70;          // governor: GFX busy % that goes straight to the top of the range
static int gPmTarget = 50;      // governor: busy % a lower cap is sized for (cap = cap * busy / target)
static int gPmStepDown = 300;   // governor: MHz the cap may fall per tick
// debug.phx_pm_cg: GFX clock gating as Linux sets it on GC 11.0.1 (gfx_v11_0_update_gfx_clock_gating): medium grain
// (RLC_CGTT_MGCG_OVERRIDE) and coarse grain + light sleep (RLC_CGCG_CGLS_CTRL and _3D), the CP's WPTR poll and GUI idle
// interrupts CGCG waits on, and SDMA0's CGCG, inside RLC safe mode. The bring-up leaves every override set, so the GFX
// clocks never stopped. Checked every 16th PM tick and set again if a GPU re-init cleared it; 0 restores the old values.
static int gPmCg = 1;
static struct {
    int state;                  // 0 not started, 1 running, -1 not SMU 13.0.4, -2 no VRAM window, -3 driver table refused, -4 gave up
    int dpmMin, dpmMax;         // the PMFW's GFX range (DpmClocks_t MinGfxClk / MaxGfxClk)
    int gfxclk, gfxclkAvg, busy, busyAvg, fclkAvg, socclk, memclk;
    int socketMw, socketMwAvg, coreMwAvg, apuMw, vddMw, socMw;
    int gfxTemp, socTemp, throttler, c0;
    int hardMin, softMax;       // what the kext last sent (0 = nothing)
    int updates, errors, vramWasZero;
    int cgState, cgApplies;     // GFX clock gating: 1 on, 0 off; times it was (re)applied
} gPm;
SYSCTL_INT(_debug, OID_AUTO, phx_pm_ms, CTLFLAG_RW | CTLFLAG_LOCKED, &gPmMs, 0, "SMU metrics period, ms (0 off)");
SYSCTL_INT(_debug, OID_AUTO, phx_pm_mode, CTLFLAG_RW | CTLFLAG_LOCKED, &gPmMode, 0,
           "GFX clock policy: 0 PMFW defaults, 1 auto range, 2 kext governor");
SYSCTL_INT(_debug, OID_AUTO, phx_pm_gfx_max, CTLFLAG_RW | CTLFLAG_LOCKED, &gPmGfxMax, 0, "GFX ceiling, MHz (0 DPM table)");
SYSCTL_INT(_debug, OID_AUTO, phx_pm_up, CTLFLAG_RW | CTLFLAG_LOCKED, &gPmUp, 0, "governor: busy % that jumps to max");
SYSCTL_INT(_debug, OID_AUTO, phx_pm_target, CTLFLAG_RW | CTLFLAG_LOCKED, &gPmTarget, 0, "governor: busy % to size the cap for");
SYSCTL_INT(_debug, OID_AUTO, phx_pm_step_down, CTLFLAG_RW | CTLFLAG_LOCKED, &gPmStepDown, 0, "governor: MHz down per tick");
#define PHX_PM_RO(name, field, desc) \
    SYSCTL_INT(_debug, OID_AUTO, name, CTLFLAG_RD | CTLFLAG_LOCKED, &gPm.field, 0, desc)
PHX_PM_RO(phx_pm_state, state, "1 running, 0 not started, <0 off (see PhxBringup log)");
PHX_PM_RO(phx_pm_dpm_min, dpmMin, "PMFW GFX minimum, MHz");
PHX_PM_RO(phx_pm_dpm_max, dpmMax, "PMFW GFX maximum, MHz");
PHX_PM_RO(phx_pm_gfxclk, gfxclk, "GFX clock, MHz");
PHX_PM_RO(phx_pm_gfxclk_avg, gfxclkAvg, "GFX clock, filtered, MHz");
PHX_PM_RO(phx_pm_busy, busy, "GFX activity, %");
PHX_PM_RO(phx_pm_busy_avg, busyAvg, "GFX activity, filtered, %");
PHX_PM_RO(phx_pm_fclk_avg, fclkAvg, "fabric clock, filtered, MHz");
PHX_PM_RO(phx_pm_socclk, socclk, "SoC clock, MHz");
PHX_PM_RO(phx_pm_memclk, memclk, "memory clock, MHz");
PHX_PM_RO(phx_pm_socket_mw, socketMw, "package power, mW");
PHX_PM_RO(phx_pm_socket_mw_avg, socketMwAvg, "package power, filtered, mW");
PHX_PM_RO(phx_pm_core_mw_avg, coreMwAvg, "CPU cores power, filtered, mW");
PHX_PM_RO(phx_pm_apu_mw, apuMw, "APU power, mW");
PHX_PM_RO(phx_pm_vdd_mw, vddMw, "VDDCR_VDD rail power, mW");
PHX_PM_RO(phx_pm_soc_mw, socMw, "VDDCR_SOC rail power, mW");
PHX_PM_RO(phx_pm_gfx_temp, gfxTemp, "GFX temperature, centi-C");
PHX_PM_RO(phx_pm_soc_temp, socTemp, "SoC temperature, centi-C");
PHX_PM_RO(phx_pm_throttler, throttler, "ThrottlerStatus bits");
PHX_PM_RO(phx_pm_c0, c0, "mean AverageCoreC0Residency of the 8 cores (PMFW units)");
PHX_PM_RO(phx_pm_hardmin, hardMin, "GFX HardMin last sent, MHz");
PHX_PM_RO(phx_pm_softmax, softMax, "GFX SoftMax last sent, MHz");
PHX_PM_RO(phx_pm_updates, updates, "metrics tables read");
PHX_PM_RO(phx_pm_errors, errors, "SMU messages that failed");
PHX_PM_RO(phx_pm_vram_was_zero, vramWasZero, "the table page was clear before first use");
PHX_PM_RO(phx_pm_cg_state, cgState, "GFX clock gating: 1 on, 0 off");
PHX_PM_RO(phx_pm_cg_applies, cgApplies, "times GFX clock gating was (re)applied");
SYSCTL_INT(_debug, OID_AUTO, phx_pm_cg, CTLFLAG_RW | CTLFLAG_LOCKED, &gPmCg, 0, "GFX clock gating (MGCG + CGCG/CGLS): 1 on, 0 off");
static struct sysctl_oid *const kPmOids[] = {
    &sysctl__debug_phx_pm_ms, &sysctl__debug_phx_pm_mode, &sysctl__debug_phx_pm_gfx_max,
    &sysctl__debug_phx_pm_up, &sysctl__debug_phx_pm_target, &sysctl__debug_phx_pm_step_down,
    &sysctl__debug_phx_pm_state, &sysctl__debug_phx_pm_dpm_min, &sysctl__debug_phx_pm_dpm_max,
    &sysctl__debug_phx_pm_gfxclk, &sysctl__debug_phx_pm_gfxclk_avg, &sysctl__debug_phx_pm_busy,
    &sysctl__debug_phx_pm_busy_avg, &sysctl__debug_phx_pm_fclk_avg, &sysctl__debug_phx_pm_socclk,
    &sysctl__debug_phx_pm_memclk, &sysctl__debug_phx_pm_socket_mw, &sysctl__debug_phx_pm_socket_mw_avg,
    &sysctl__debug_phx_pm_core_mw_avg, &sysctl__debug_phx_pm_apu_mw, &sysctl__debug_phx_pm_vdd_mw,
    &sysctl__debug_phx_pm_soc_mw, &sysctl__debug_phx_pm_gfx_temp, &sysctl__debug_phx_pm_soc_temp,
    &sysctl__debug_phx_pm_throttler, &sysctl__debug_phx_pm_c0, &sysctl__debug_phx_pm_hardmin,
    &sysctl__debug_phx_pm_softmax, &sysctl__debug_phx_pm_updates, &sysctl__debug_phx_pm_errors,
    &sysctl__debug_phx_pm_vram_was_zero, &sysctl__debug_phx_pm_cg_state, &sysctl__debug_phx_pm_cg_applies,
    &sysctl__debug_phx_pm_cg,
};
// GC 11.0.1 registers (segment 0 base 0x1260, segment 1 base 0xa000)
static constexpr uint32_t kRegSqCmd = 0x1260 + 0x111b, kRegRlcSafeMode = 0xa000 + 0x0980;
static volatile UInt32 gSysctlUp;

// psp_gfx_rb_frame (64 B) — the ring entry the PSP reads. Only 5 fields are set; the rest stay zero
// (Linux amdgpu_psp.c:3485: a non-zero cmd_buf_size makes the PSP silently drop the frame).
struct PhxGfxRbFrame {
    uint32_t cmd_buf_addr_lo, cmd_buf_addr_hi, cmd_buf_size;
    uint32_t fence_addr_lo, fence_addr_hi, fence_value;
    uint32_t sid_lo, sid_hi;
    uint8_t vmid, frame_type, reserved1[2];
    uint32_t reserved2[7];
} __attribute__((packed));
static_assert(sizeof(PhxGfxRbFrame) == 64, "PhxGfxRbFrame must be 64 B");

// Canned "phoenix" table (Mesa amdgpu_devices.c): GB_ADDR_CONFIG and GC 11.0.1.
static constexpr uint32_t kGbAddrConfig = 0x00000242;
static constexpr uint32_t kGcVersion = (11u << 16) | (0u << 8) | 1u;

// Pools reported to RADV. E1 backs every BO with system memory, so these are budgets, not hardware:
// BAR0 is 256 MiB on this machine, the UMA carve-out 6 GiB.
static constexpr uint64_t kVisTotal = 256ull << 20;
static constexpr uint64_t kHiTotal = 1536ull << 20;
static constexpr uint64_t kGttCap = 512ull << 20;
static constexpr uint64_t kGttMaxBo = 256ull << 20;
static constexpr uint64_t kHostCap = 4096ull << 20, kHostMaxBo = 1024ull << 20;   // host imports, per client
static constexpr uint64_t kVaMask = (1ull << 48) - 1;
static constexpr int kMaxMaps = 4096;

static bool vaCanonical(uint64_t va) {
    uint64_t top = va >> 47;
    return top == 0 || top == 0x1FFFF;
}

class PhxBringup : public IOService {
    OSDeclareDefaultStructors(PhxBringup)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override;

    bool claimSession();
    void releaseSession();
    IOReturn readRegs(uint32_t first, uint32_t count, uint32_t *out);
    IOReturn vramRead(uint32_t byteOff, uint32_t count, uint32_t *out);
    IOReturn writeReg(uint32_t reg, uint32_t value, uint32_t *readback);
    IOReturn vramWrite(uint32_t byteOff, uint32_t value);
    IOReturn doorbellWrite(uint64_t byteOff, uint64_t value);
    IOReturn sysMem(uint64_t op, uint64_t a, uint64_t b, uint64_t *out0, uint64_t *out1);
    IOReturn smuMessage(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *argAfter);
    IOReturn pspRing(uint32_t op, uint64_t carveOffset, uint64_t out[4]);
    IOReturn pspSubmit(uint32_t cmdId, uint64_t out[4]);
    IOReturn fwStage(uint32_t dstOffset, const void *bytes, uint32_t len);
    IOReturn loadIpFw(uint32_t fwType, uint32_t fwBufOffset, uint32_t size, uint64_t out[4]);
    IOReturn pspCtl(uint32_t op, uint32_t arg, uint64_t out[4]);
    IOReturn pspRaw(uint32_t cmdId, const uint32_t words[6], uint64_t out[4]);
    bool debugEnabled() const { return debug; }
    // E8: GPU execution. The userspace bring-up maps MES queues (compute = 0, gfx = 1) and hands them over (adoptQueue);
    // user BOs are then mapped into the VMID 8 page table and SUBMIT writes INDIRECT_BUFFERs into the queue's ring.
    IOReturn adoptQueue(const uint64_t in[6]);
    IOReturn claimAccel();
    bool gpuReady() const { return gq[0].ready || gq[1].ready; }
    // one GPUVM context (VMID 8..15) per client process: RADV instances allocate VAs independently
    uint32_t vmidAlloc();
    void vmidFree(uint32_t vmid, uint64_t lastSeq);
    IOReturn vmMap(uint32_t vmid, uint64_t va, IOMemoryDescriptor *md, uint64_t off, uint64_t size);
    IOReturn vmUnmap(uint32_t vmid, uint64_t va, uint64_t size);
    IOReturn gpuSubmit(uint32_t vmid, bool gfx, const uint64_t *ibVa, const uint32_t *ibDw, uint32_t n, uint64_t *seq);
    uint64_t gpuRetired() const;
    const phx_topology *capturedTopology() const { return topology; }
    IOReturn setPowerState(unsigned long ordinal, IOService *whichDevice) override;

private:
    uint32_t rreg(uint32_t reg) const;
    void wreg(uint32_t reg, uint32_t value) const;
    uint32_t smuPoll() const;
    bool pspWaitTos(uint32_t timeoutMs) const;
    IOReturn pspRunCmd(uint32_t cmdId, const uint32_t *unionWords, uint32_t nWords, uint64_t out[4]);
    void hdpFlush() const;
    void readFbLayout();
    void clearRing();

    const phx_topology *topology{nullptr};
    bool isSleeping{false};
    IOPCIDevice *pci{nullptr};
    IOMemoryMap *mmio{nullptr};
    IOLock *smuLock{nullptr};
    // UMA carve-out ("VRAM"): GPU MC address range and the system physical address behind it.
    uint64_t fbMcBase{0}, fbSize{0}, fbPhys{0};
    // BAR0 framebuffer aperture: the CPU window into the first fbAperLen bytes of VRAM. It is an MMIO
    // region above system RAM (phys 0x8_2040_0000 here, 256 MiB), so CPU writes through it reach the
    // GPU's local memory and never the macOS heap. This is how the PSP ring is initialised.
    IOMemoryMap *fbAper{nullptr};
    IOMemoryMap *doorbellMap{nullptr}; // BAR2: doorbell aperture (2 MiB)
    IOBufferMemoryDescriptor *sysBuf{nullptr}; // debug: physically contiguous system memory for GPU VM experiments
    uint64_t fbAperPhys{0}, fbAperLen{0};
    // MMIO dword to write 0 to for an HDP flush (0 if the NBIO remap register was implausible).
    uint32_t hdpFlushDword{0};
    // PSP KM ring (debug selector 101): MC address once created, else 0. The GPCOM command + fence buffers
    // (selector 103) live just above the ring in VRAM; offsets are VRAM byte offsets, Mc the matching MC address.
    uint64_t ringMc{0}, ringOff{0};
    uint64_t cmdOff{0}, cmdMc{0}, fenceOff{0}, fenceMc{0};
    uint32_t fenceCounter{0};
    // Firmware bring-up regions (set when the ring is created): TMR the PSP copies firmware into, and fw_buf
    // where the CPU stages ucode for LOAD_IP_FW. Offsets are VRAM byte offsets, Mc the matching MC addresses.
    uint64_t tmrOff{0}, tmrMc{0}, tmrSize{0};
    uint64_t fwBufOff{0}, fwBufMc{0}, fwBufSize{0};
    bool tmrSetUp{false};
    // E8 state: the adopted queues (ring/wb/fence are VRAM byte offsets reached through BAR0) and the VMID 8
    // page-table pool (physically contiguous system memory; page 0 is the root). Sequence numbers are global across
    // the queues; each queue's fence holds the last sequence it finished, and fseq/fend keep its outstanding
    // submissions in order (sequence, ring position after it).
    struct GpuQueue {
        bool ready, gfx;
        uint32_t ringOff, ringDw, wbOff, fenceOff;
        uint64_t dbByte, wptr, doneEnd;
        uint64_t fseq[64], fend[64];
        uint32_t fvmid[64];                     // the hardware VMID each outstanding submission runs under
        volatile uint32_t head, tail;
        uint64_t lastDone, lastProgress;        // the hang watchdog's view: fence seen last, and when it last moved
        uint64_t killedSeq; bool killedAll;
    };
    GpuQueue gq[2]{};
    uint64_t gSeq{0};
    uint64_t queueFence(const GpuQueue &q) const;
    IOLock *gpuLock{nullptr};
    // RLC safe mode is one state for the whole GFX block: the hang watchdog (killWaves) and the PM tick (cgPolicy) run
    // on separate thread calls, so entering and leaving it is serialized.
    IOLock *rlcLock{nullptr};
    void rlcSafeMode(bool enter) const;
    bool cgOn{false}, cgSaved{false};
    uint32_t cgOld[6]{};
    uint32_t cgChecks{0};
    void cgPolicy();
    // One GPUVM context (page tables) per client, and the eight hardware VMIDs lent to contexts at submission time,
    // as amdgpu does: a fixed VMID per client capped Metal at eight processes, and with every app allowed Metal the
    // ninth got no device (2026-10-20). A context's handle is its index + 1.
    // A context's page tables, in physically contiguous 64 KiB chunks allocated as it maps (most contexts need one or
    // two): a single 4 MiB contiguous pool per context failed once memory was fragmented. Table i is page i % 16 of
    // chunk i / 16; table 0 is the root.
    static constexpr uint32_t kPtChunkBytes = 64u << 10, kPtChunkPages = kPtChunkBytes / 4096, kPtChunks = 64;
    struct VmCtx {
        IOBufferMemoryDescriptor *chunk[kPtChunks];
        uint64_t chunkPhys[kPtChunks];
        uint64_t phys;                    // the root table
        uint32_t pages, used;
        bool busy;
        uint32_t hw;                      // the hardware VMID pointed at these tables, 0 = none
    };
    static constexpr uint32_t kMaxCtx = 96;
    VmCtx vm[kMaxCtx]{};
    struct HwVmid {
        uint32_t ctx;                     // handle of the context it points at, 0 = free
        uint64_t lastSeq, lastUse;        // its newest submission; when it was last bound or used
    };
    HwVmid hwv[8]{};
    uint64_t hwTick{0};
    uint32_t vmBind(uint32_t handle);
    VmCtx *vmCtx(uint32_t handle) { return handle && handle <= kMaxCtx && vm[handle - 1].busy ? &vm[handle - 1] : nullptr; }
    uint64_t *ptTable(const VmCtx &c, uint64_t phys) const;
    uint64_t ptTablePhys(VmCtx &c, uint32_t i);
    uint64_t *vmPte(VmCtx &c, uint64_t va, bool alloc);
    void vmFlushTlb(uint32_t vmid) const;
    void ringPut(GpuQueue &q, uint32_t idx, uint32_t value);
    thread_call_t hangCall{nullptr};
    volatile bool hangStop{false};
    static void hangTick(thread_call_param_t self, thread_call_param_t);
    void hangCheck();
    void killWaves(uint32_t vmid, bool all);
    // SMU 13.0.4 power management (debug.phx_pm_*): metrics telemetry and the GFX clock policy, on their own timer
    thread_call_t pmCall{nullptr};
    volatile bool pmStop{false}, pmResync{false};
    bool pmReady{false}, pmDead{false};
    uint16_t pciDeviceId{0};
    uint32_t pmFails{0}, pmResets{0}, pmTicks{0};
    int pmModeApplied{0}, pmCap{0};
    static void pmTick(thread_call_param_t self, thread_call_param_t);
    void pmStep();
    bool pmSetup();
    IOReturn pmTransfer(uint32_t tableId, void *out, uint32_t len);
    bool pmSend(uint32_t msg, uint32_t arg);
    bool pmSetRange(int lo, int hi);
    void pmPolicy(const phx_smu13_0_4_metrics &m);
    bool debug{false};
    bool smuUsed{false};
    volatile SInt32 sessionOpen{0};
};

OSDefineMetaClassAndStructors(PhxBringup, IOService)

class IOAccelPhxNativeClient : public IOUserClient {
    OSDeclareDefaultStructors(IOAccelPhxNativeClient)
public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;

private:
    struct Bo {
        IOMemoryDescriptor *md;   // an IOBufferMemoryDescriptor, or the caller's own pages (host, prepared)
        uint64_t size;
        uint32_t domain;
        uint32_t bits;
        uint8_t *kva;             // the kernel's view of the BO; NULL for host imports, which the kernel never reads
        bool host;
    };
    struct Map {
        bool used;
        uint32_t handle;
        uint64_t va, size, off;
        uint32_t flags;
    };

    IOReturn doHello(IOExternalMethodArguments *a);
    IOReturn doQueryInfo(IOExternalMethodArguments *a);
    IOReturn doTopology(IOExternalMethodArguments *a);
    IOReturn doReadRegs(IOExternalMethodArguments *a);
    IOReturn doBoCreate(IOExternalMethodArguments *a);
    IOReturn doBoFree(IOExternalMethodArguments *a);
    IOReturn doBoImportHost(IOExternalMethodArguments *a);
    void boRelease(Bo &b);
    IOReturn doGemVa(IOExternalMethodArguments *a);
    IOReturn doCtx(IOExternalMethodArguments *a);
    IOReturn doSubmit(IOExternalMethodArguments *a);
    IOReturn doWaitSeq(IOExternalMethodArguments *a);
    IOReturn doSurfGpuMark(IOExternalMethodArguments *a);
    IOReturn doDbgSmu(IOExternalMethodArguments *a);
    IOReturn doDbgPspRing(IOExternalMethodArguments *a);
    IOReturn doDbgVramRead(IOExternalMethodArguments *a);
    IOReturn doDbgPspSubmit(IOExternalMethodArguments *a);
    IOReturn doDbgFwStage(IOExternalMethodArguments *a);
    IOReturn doDbgLoadIpFw(IOExternalMethodArguments *a);
    IOReturn doDbgPspCtl(IOExternalMethodArguments *a);
    IOReturn doDbgPspRaw(IOExternalMethodArguments *a);
    IOReturn doDbgWriteReg(IOExternalMethodArguments *a);
    IOReturn doDbgVramWrite(IOExternalMethodArguments *a);
    IOReturn doDbgDoorbell(IOExternalMethodArguments *a);
    IOReturn doDbgSysMem(IOExternalMethodArguments *a);
    IOReturn doDbgAdoptQueue(IOExternalMethodArguments *a);
    IOReturn doDbgClaim(IOExternalMethodArguments *a);
    void flushBos();
    Bo *findBo(uint32_t h);
    bool ibCovered(uint64_t vaCanon, uint64_t bytes);
    void freeAll();

    PhxBringup *owner{nullptr};
    IOLock *lock{nullptr};
    bool privileged{false};
    bool hello{false};
    bool sessionClaimed{false};
    uint32_t vmid{0};        // this client's GPUVM context, taken on its first GPU mapping or submission
    uint64_t lastSeq{0};     // its newest submission: the context is released only once the GPU is past it
    bool ensureVmid() { return vmid || (vmid = owner->vmidAlloc()) != 0; }
    Bo *bos{nullptr};      // PHXN_MAX_BOS entries, index = handle
    Map *maps{nullptr};    // kMaxMaps entries
    bool ctx[PHXN_MAX_CTX + 1]{};
    uint64_t visUsed{0}, hiUsed{0}, gttUsed{0}, hostUsed{0};
    task_t task{nullptr};
    uint64_t emitted{0};
    uint32_t pendFenceHandle{0}, pendFenceOffset{0};
    uint64_t pendFenceSeq{0};
};

OSDefineMetaClassAndStructors(IOAccelPhxNativeClient, IOUserClient)

// ---- service ---------------------------------------------------------------------------------------------

bool PhxBringup::start(IOService *provider) {
    int dummy = 0;
    if (PE_parse_boot_argn("-phxbringupoff", &dummy, sizeof(dummy)))
        return false;
    if (!IOService::start(provider))
        return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return false;
    pci->retain();
    {
        uint16_t v = pci->configRead16(0);
        uint16_t d = pci->configRead16(2);
        pciDeviceId = d;
        uint8_t rev = pci->configRead8(8);
        uint16_t sv = pci->configRead16(0x2c);
        uint16_t sd = pci->configRead16(0x2e);
        topology = phx_find_topology(v, d, rev, sv, sd);
        if (topology) {
            setProperty("phx-topology-cu", topology->cu_active_number, 32);
            setProperty("phx-topology-rb", topology->num_rb_pipes, 32);
            IOLog("PhxBringup: topology matched: %u active CUs, %u RBs for device 0x%04x:0x%04x\n",
                  topology->cu_active_number, topology->num_rb_pipes, v, d);
        }
    }
    PMinit();
    provider->joinPMtree(this);
    static IOPMPowerState pmStates[2] = {
        { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, IOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
    };
    registerPowerDriver(this, pmStates, 2);
    {   // the GPU must be a bus master before it can DMA to/from system memory (page tables, user BOs)
        uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
        setProperty("phx-pci-cmd-before", cmd, 16);
        pci->setBusMasterEnable(true);
        pci->setMemoryEnable(true);
        setProperty("phx-pci-cmd-after", pci->configRead16(kIOPCIConfigCommand), 16);
    }
    mmio = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
    fbAper = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (fbAper) {
        fbAperLen = fbAper->getLength();
        fbAperPhys = fbAper->getPhysicalAddress();
    }
    doorbellMap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    smuLock = IOLockAlloc();
    gpuLock = IOLockAlloc();
    rlcLock = IOLockAlloc();
    uint32_t dbg = 0;
    debug = PE_parse_boot_argn("phxdebug", &dbg, sizeof(dbg)) && dbg == 1;
    if (mmio) {
        readFbLayout();
        // HDP flush window: the NBIO remap register holds the BAR5 byte offset where writing 0 flushes the
        // host data path. Accept it only if it is a plausible dword offset inside our MMIO aperture.
        uint32_t remap = rreg(PHX_NBIO_REMAP_HDP_MEM_FLUSH_CNTL);
        if (remap && remap != 0xFFFFFFFFu && (remap & 3) == 0 && remap + 4 <= mmio->getLength())
            hdpFlushDword = remap >> 2;
        setProperty("phx-hdp-flush-dword", hdpFlushDword, 32);
    }
    setProperty("phx-build", PHX_KEXT_BUILD, 32);
    setProperty("phx-stage", "E3");
    setProperty("phx-debug", debug);
    setProperty("phx-fb-aper-phys", fbAperPhys, 64);
    setProperty("phx-fb-aper-len", fbAperLen, 64);
    IOLog("PhxBringup: service up (mmio %s, bar0 %s %llu MiB, debug %d)\n", mmio ? "mapped" : "unmapped",
          fbAper ? "mapped" : "unmapped", (unsigned long long)(fbAperLen >> 20), debug);
    hangCall = thread_call_allocate(hangTick, this);
    if (hangCall) {
        uint64_t d;
        clock_interval_to_deadline(500, kMillisecondScale, &d);
        thread_call_enter_delayed(hangCall, d);
    }
    // power management starts 15 s in: telemetry only unless debug.phx_pm_mode asks for more
    pmCall = thread_call_allocate(pmTick, this);
    if (pmCall) {
        uint64_t d;
        clock_interval_to_deadline(15000, kMillisecondScale, &d);
        thread_call_enter_delayed(pmCall, d);
    }
    registerService();
    return true;
}

void PhxBringup::stop(IOService *provider) {
    if (hangCall) {
        hangStop = true;   // a tick running now does not arm the next one
        thread_call_cancel_wait(hangCall);
        thread_call_free(hangCall);
        hangCall = nullptr;
    }
    if (pmCall) {
        pmStop = true;
        thread_call_cancel_wait(pmCall);
        thread_call_free(pmCall);
        pmCall = nullptr;
        if (pmReady && pmModeApplied && gPm.dpmMax)
            pmSetRange(gPm.dpmMin, gPm.dpmMax);   // hand the PMFW its own range back
    }
    if (gSysctlUp) {
        sysctl_unregister_oid(&sysctl__debug_phxbos_flush);
        sysctl_unregister_oid(&sysctl__debug_phxbos_flush_bytes);
        sysctl_unregister_oid(&sysctl__debug_phx_hang_ms);
        sysctl_unregister_oid(&sysctl__debug_phx_hang_kills);
        for (struct sysctl_oid *o : kPmOids)
            sysctl_unregister_oid(o);
        gSysctlUp = 0;
    }
    clearRing();
    if (fbAper) {
        fbAper->release();
        fbAper = nullptr;
    }
    if (sysBuf) {
        sysBuf->release();
        sysBuf = nullptr;
    }
    gq[0].ready = gq[1].ready = false;
    for (VmCtx &c : vm) {
        for (IOBufferMemoryDescriptor *b : c.chunk)
            if (b)
                b->release();
        c = VmCtx{};
    }
    for (HwVmid &h : hwv)
        h = HwVmid{};
    if (gpuLock) {
        IOLockFree(gpuLock);
        gpuLock = nullptr;
    }
    if (rlcLock) {
        IOLockFree(rlcLock);
        rlcLock = nullptr;
    }
    if (doorbellMap) {
        doorbellMap->release();
        doorbellMap = nullptr;
    }
    if (mmio) {
        mmio->release();
        mmio = nullptr;
    }
    if (pci) {
        pci->release();
        pci = nullptr;
    }
    if (smuLock) {
        IOLockFree(smuLock);
        smuLock = nullptr;
    }
    PMstop();
    IOService::stop(provider);
}

IOReturn PhxBringup::setPowerState(unsigned long ordinal, IOService *whichDevice) {
    if (ordinal == 0) {
        IOLog("PhxBringup: powerStateWillChange to sleep (state 0)\n");
        isSleeping = true;
    } else {
        IOLog("PhxBringup: powerStateDidChange from sleep (state 1)\n");
        pmResync = true;          // the SMU may have lost the driver table address and our limits
        isSleeping = false;
    }
    return IOPMAckImplied;
}

uint32_t PhxBringup::rreg(uint32_t reg) const {
    return reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress())[reg];
}

void PhxBringup::wreg(uint32_t reg, uint32_t value) const {
    reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress())[reg] = value;
}

// FB location (MC, 16 MiB units, FB_BASE bits 23:0) and the system physical address of the carve-out (FB_OFFSET << 24).
void PhxBringup::readFbLayout() {
    uint32_t base = rreg(PHX_MMMC_VM_FB_LOCATION_BASE) & 0x00FFFFFF;
    uint32_t top = rreg(PHX_MMMC_VM_FB_LOCATION_TOP) & 0x00FFFFFF;
    fbMcBase = static_cast<uint64_t>(base) << 24;
    fbSize = top >= base ? (static_cast<uint64_t>(top - base) + 1) << 24 : 0;
    fbPhys = static_cast<uint64_t>(rreg(PHX_MMMC_VM_FB_OFFSET)) << 24;
    setProperty("phx-fb-mc-base", fbMcBase, 64);
    setProperty("phx-fb-size", fbSize, 64);
    setProperty("phx-fb-phys", fbPhys, 64);
    // About This Mac / System Information name the GPU from the PCI device's "model" and size its memory from
    // "VRAM,totalMB"; without them the 780M read "Display, 15 MB" (the boot framebuffer)
    static const char kModel780[] = "AMD Radeon 780M";
    static const char kModel760[] = "AMD Radeon 760M";
    static const char kModel740[] = "AMD Radeon 740M";
    const char *model = kModel780;
    unsigned modelSize = sizeof(kModel780);
    if (topology) {
        if (topology->cu_active_number <= 4) {
            model = kModel740; modelSize = sizeof(kModel740);
        } else if (topology->cu_active_number <= 8) {
            model = kModel760; modelSize = sizeof(kModel760);
        }
    }
    if (OSData *m = OSData::withBytes(model, modelSize)) {
        pci->setProperty("model", m);
        m->release();
    }
    if (fbSize)
        pci->setProperty("VRAM,totalMB", fbSize >> 20, 32);
}

void PhxBringup::clearRing() {
    ringMc = ringOff = 0;
    cmdOff = cmdMc = fenceOff = fenceMc = 0;
    fenceCounter = 0;
    tmrOff = tmrMc = tmrSize = 0;
    fwBufOff = fwBufMc = fwBufSize = 0;
    tmrSetUp = false;
}

// Drain the host data path so the PSP sees our VRAM writes before we ring the doorbell (C2PMSG_67).
void PhxBringup::hdpFlush() const {
    if (hdpFlushDword)
        wreg(hdpFlushDword, 0);
    (void)rreg(0x0DE3);   // read-back barrier (GRBM_STATUS), as in the reference
}

// Read-only: count dwords from VRAM at byteOff, through the BAR0 aperture. Used by debug selector 102 to
// find the live console framebuffer and a free (all-zero) VRAM region for the PSP structures.
IOReturn PhxBringup::vramRead(uint32_t byteOff, uint32_t count, uint32_t *out) {
    if (!fbAper)
        return kIOReturnNotReady;
    if ((byteOff & 3) || (static_cast<uint64_t>(byteOff) + static_cast<uint64_t>(count) * 4) > fbAperLen)
        return kIOReturnBadArgument;
    volatile uint32_t *v = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + byteOff);
    for (uint32_t i = 0; i < count; i++)
        out[i] = v[i];
    return kIOReturnSuccess;
}

// Write one MMIO dword (bring-up experimentation; root + phxdebug only). Returns the readback.
IOReturn PhxBringup::writeReg(uint32_t reg, uint32_t value, uint32_t *readback) {
    if (!mmio)
        return kIOReturnNotReady;
    if ((static_cast<uint64_t>(reg) + 1) * 4 > mmio->getLength())
        return kIOReturnBadArgument;
    wreg(reg, value);
    __asm__ __volatile__("mfence" ::: "memory");
    *readback = rreg(reg);
    return kIOReturnSuccess;
}

// Write one dword into VRAM at byteOff through the BAR0 aperture.
// op 1: allocate a physically contiguous buffer of a bytes (replaces any previous), out0 = physical address
// op 2: read dword at byte offset a -> out1;  op 3: write dword b at byte offset a;  op 4: free
IOReturn PhxBringup::sysMem(uint64_t op, uint64_t a, uint64_t b, uint64_t *out0, uint64_t *out1) {
    *out0 = *out1 = 0;
    if (op == 1) {
        if (sysBuf) {
            sysBuf->release();
            sysBuf = nullptr;
        }
        if (a == 0 || a > (64ull << 20))
            return kIOReturnBadArgument;
        sysBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, a, 0x00007FFFFFFFF000ull);
        if (!sysBuf)
            return kIOReturnNoMemory;
        bzero(sysBuf->getBytesNoCopy(), a);
        *out0 = sysBuf->getPhysicalAddress();
        *out1 = sysBuf->getLength();
        return kIOReturnSuccess;
    }
    if (!sysBuf)
        return kIOReturnNotReady;
    if (op == 4) {
        sysBuf->release();
        sysBuf = nullptr;
        return kIOReturnSuccess;
    }
    if ((a & 3) || a + 4 > sysBuf->getLength())
        return kIOReturnBadArgument;
    volatile uint32_t *p = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(sysBuf->getBytesNoCopy()) + a);
    if (op == 2) {
        __asm__ __volatile__("clflush (%0)\n\tmfence" ::"r"(p) : "memory");
        *out1 = *p;
        *out0 = sysBuf->getPhysicalAddress();
        return kIOReturnSuccess;
    }
    if (op == 3) {
        *p = static_cast<uint32_t>(b);
        __asm__ __volatile__("clflush (%0)\n\tmfence" ::"r"(p) : "memory"); // GPU walker may not snoop CPU caches
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}

// ---- E8: GPU execution ------------------------------------------------------------------------------------
// Everything here reproduces what tools/phxctl/mesboot.c proved on the 780M: VMID 8 with a 4-level table in
// system memory (PDE = next|VALID|SYSTEM|SNOOPED, no BFS/TF since gmc_v11 has translate_further off), system-page
// PTEs with MTYPE 4, invalidation on engine 17, and an ACQUIRE/RELEASE_MEM GL2 writeback after each IB.
static constexpr uint32_t kFirstUserVmid = 8, kNumUserVmids = 8;   // VMIDs 8..15, lent to client contexts (vmBind)
static constexpr uint64_t kPteSysFlags = 0x77ull | (4ull << 48);  // VALID SYSTEM SNOOPED EXEC READ WRITE, MTYPE 4
static constexpr uint64_t kPtAddrMask = 0x0000FFFFFFFFF000ull;
// GC registers (absolute dword index, GC base 0x1260): context n = context 0 + n (CNTL) / + 2n (address pairs),
// invalidation engine 17
static constexpr uint32_t kRegCtx0Cntl = 0x28E8, kRegCtx0BaseLo = 0x2953, kRegCtx0StartLo = 0x2973, kRegCtx0EndLo = 0x2993;
static constexpr uint32_t kRegInvEng17Req = 0x291C, kRegInvEng17Ack = 0x292E;

static inline void flushLine(const volatile void *p) {
    __asm__ __volatile__("clflush (%0)" ::"r"(p) : "memory");
}

uint64_t *PhxBringup::ptTable(const VmCtx &c, uint64_t phys) const {
    for (uint32_t i = 0; i < kPtChunks && c.chunk[i]; i++)
        if (phys - c.chunkPhys[i] < kPtChunkBytes)
            return reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(c.chunk[i]->getBytesNoCopy()) + (phys - c.chunkPhys[i]));
    return nullptr;
}

// The physical address of table i, allocating (zeroed, flushed) the chunk it lives in; 0 when that fails.
uint64_t PhxBringup::ptTablePhys(VmCtx &c, uint32_t i) {
    const uint32_t k = i / kPtChunkPages;
    if (k >= kPtChunks)
        return 0;
    if (!c.chunk[k]) {
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, kPtChunkBytes, kPtAddrMask);
        if (!b)
            return 0;
        uint8_t *p = static_cast<uint8_t *>(b->getBytesNoCopy());
        bzero(p, kPtChunkBytes);
        for (uint32_t o = 0; o < kPtChunkBytes; o += 64)
            flushLine(p + o);
        c.chunk[k] = b;
        c.chunkPhys[k] = b->getPhysicalAddress();
    }
    return c.chunkPhys[k] + static_cast<uint64_t>(i % kPtChunkPages) * 4096;
}

uint64_t *PhxBringup::vmPte(VmCtx &c, uint64_t va, bool alloc) {
    uint64_t tbl = c.phys;
    for (int shift = 39; shift >= 21; shift -= 9) {
        uint64_t *t = ptTable(c, tbl);
        if (!t)
            return nullptr;
        uint64_t *e = t + ((va >> shift) & 511);
        if (!(*e & 1)) {
            if (!alloc || c.used >= c.pages)
                return nullptr;
            const uint64_t child = ptTablePhys(c, c.used);
            if (!child)
                return nullptr;
            c.used++;
            *e = child | 7;
            flushLine(e);
        }
        tbl = *e & kPtAddrMask;
    }
    uint64_t *t = ptTable(c, tbl);
    return t ? t + ((va >> 12) & 511) : nullptr;
}

void PhxBringup::vmFlushTlb(uint32_t vmid) const {
    __asm__ __volatile__("mfence" ::: "memory");
    wreg(kRegInvEng17Req, 0x00F80000u | (1u << vmid) | 1u);
    for (int i = 0; i < 1000 && !(rreg(kRegInvEng17Ack) & (1u << vmid)); i++)
        IODelay(1);
}

// Takes a free context: empty tables (root only), no hardware VMID until its first submission. Returns its handle,
// 0 when all kMaxCtx are in use.
uint32_t PhxBringup::vmidAlloc() {
    if (!gpuReady())
        return 0;
    IOLockLock(gpuLock);
    for (uint32_t i = 0; i < kMaxCtx; i++) {
        VmCtx &c = vm[i];
        if (c.busy)
            continue;
        if (!c.chunk[0]) {
            c.phys = ptTablePhys(c, 0);
            if (!c.phys)
                break;
            c.pages = kPtChunks * kPtChunkPages;
        }
        c.used = 1;
        c.busy = true;
        c.hw = 0;
        IOLockUnlock(gpuLock);
        return i + 1;
    }
    IOLockUnlock(gpuLock);
    return 0;
}

// Points a hardware VMID at the context's tables for a submission (gpuLock held): the one it already has, a free one,
// or the least recently used one whose submissions the GPU has all finished (waits for one, bounded ~2 s). 0 on timeout.
uint32_t PhxBringup::vmBind(uint32_t handle) {
    VmCtx &c = vm[handle - 1];
    const uint64_t now = ++hwTick;
    if (c.hw) {
        hwv[c.hw - kFirstUserVmid].lastUse = now;
        return c.hw;
    }
    int pick = -1;
    for (int spin = 0; pick < 0; spin++) {
        const uint64_t done = gpuRetired();
        uint64_t oldest = ~0ull;
        for (uint32_t i = 0; i < kNumUserVmids; i++) {
            if (!hwv[i].ctx) {
                pick = static_cast<int>(i);
                break;
            }
            if (hwv[i].lastSeq <= done && hwv[i].lastUse < oldest) {
                oldest = hwv[i].lastUse;
                pick = static_cast<int>(i);
            }
        }
        if (pick >= 0)
            break;
        if (spin > 40000)
            return 0;
        IODelay(50);
    }
    HwVmid &h = hwv[pick];
    if (h.ctx)
        vm[h.ctx - 1].hw = 0;
    h.ctx = handle;
    h.lastSeq = 0;
    h.lastUse = now;
    const uint32_t vmid = kFirstUserVmid + static_cast<uint32_t>(pick);
    c.hw = vmid;
    wreg(kRegCtx0BaseLo + 2 * vmid, static_cast<uint32_t>(c.phys | 3));
    wreg(kRegCtx0BaseLo + 2 * vmid + 1, static_cast<uint32_t>(c.phys >> 32));
    wreg(kRegCtx0Cntl + vmid, 0x01fffe07);
    wreg(kRegCtx0StartLo + 2 * vmid, 0);
    wreg(kRegCtx0StartLo + 2 * vmid + 1, 0);
    wreg(kRegCtx0EndLo + 2 * vmid, 0xffffffff);
    wreg(kRegCtx0EndLo + 2 * vmid + 1, 0xf);
    vmFlushTlb(vmid);
    return vmid;
}

// Returns a context once the GPU is past the client's last submission (bounded wait): the tables used so far are
// cleared, so nothing the client mapped stays reachable, and its hardware VMID (if any) is freed.
void PhxBringup::vmidFree(uint32_t handle, uint64_t lastSeq) {
    if (!vmCtx(handle))
        return;
    for (int i = 0; i < 2000 && gpuRetired() < lastSeq; i++)
        IOSleep(1);
    IOLockLock(gpuLock);
    VmCtx &c = vm[handle - 1];
    for (uint32_t k = 0; k < kPtChunks && c.chunk[k] && k * kPtChunkPages < c.used; k++) {
        const uint32_t n = c.used - k * kPtChunkPages < kPtChunkPages ? c.used - k * kPtChunkPages : kPtChunkPages;
        uint8_t *p = static_cast<uint8_t *>(c.chunk[k]->getBytesNoCopy());
        bzero(p, static_cast<size_t>(n) * 4096);
        for (uint32_t o = 0; o < n * 4096u; o += 64)
            flushLine(p + o);
    }
    if (c.hw) {
        vmFlushTlb(c.hw);
        hwv[c.hw - kFirstUserVmid].ctx = 0;
        c.hw = 0;
    }
    c.used = 1;
    c.busy = false;
    IOLockUnlock(gpuLock);
}

IOReturn PhxBringup::adoptQueue(const uint64_t in[6]) {
    if (!fbAper || !doorbellMap || !mmio || !fbMcBase || !gpuLock)
        return kIOReturnNotReady;
    // in[5] = fence offset | queue index (0 compute, 1 gfx): the fence is 8-byte aligned, so bit 0 is free
    const uint64_t ringOff = in[0], ringDw = in[1], wbOff = in[2], dbByte = in[3], wptr = in[4], fenceOff = in[5] & ~1ull;
    const uint32_t qi = static_cast<uint32_t>(in[5] & 1);
    if (ringDw < 256 || ringDw > 65536 || (ringDw & (ringDw - 1)) || (ringOff & 0xFF) ||
        ringOff + ringDw * 4 > fbAperLen || (wbOff & 7) || wbOff + 32 > fbAperLen || (fenceOff & 7) ||
        fenceOff + 8 > fbAperLen || (dbByte & 7) || dbByte + 8 > doorbellMap->getLength())
        return kIOReturnBadArgument;
    IOLockLock(gpuLock);
    GpuQueue &q = gq[qi];
    bzero(&q, sizeof(q));
    q.gfx = qi == 1;
    q.ringOff = static_cast<uint32_t>(ringOff);
    q.ringDw = static_cast<uint32_t>(ringDw);
    q.wbOff = static_cast<uint32_t>(wbOff);
    q.fenceOff = static_cast<uint32_t>(fenceOff);
    q.dbByte = dbByte;
    q.wptr = q.doneEnd = wptr;
    volatile uint32_t *fence = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + fenceOff);
    fence[0] = static_cast<uint32_t>(gSeq);
    fence[1] = static_cast<uint32_t>(gSeq >> 32);
    hdpFlush();
    q.ready = true;
    IOLockUnlock(gpuLock);
    IOLog("PhxBringup: adopted %s queue ring=+0x%llx (%llu dw) wptr=%llu doorbell=+0x%llx\n",
          qi ? "gfx" : "compute", ringOff, ringDw, wptr, dbByte);
    return kIOReturnSuccess;
}

// PhxAccel (the NullMoth accelerator in its AMD configuration) starts only on a GPU marked as driven; matching is
// re-run so the accelerator personality, refused at boot, is offered the device again.
IOReturn PhxBringup::claimAccel() {
    if (!pci || !gpuReady())
        return kIOReturnNotReady;
    pci->setProperty("nmxgpu-claimed", kOSBooleanTrue);
    pci->registerService();
    IOLog("PhxBringup: GPU claimed for PhxAccel, matching re-run\n");
    return kIOReturnSuccess;
}

IOReturn PhxBringup::vmMap(uint32_t handle, uint64_t va, IOMemoryDescriptor *md, uint64_t off, uint64_t size) {
    if (!vmCtx(handle))
        return kIOReturnNotReady;
    VmCtx &c = vm[handle - 1];
    IOReturn r = kIOReturnSuccess;
    IOLockLock(gpuLock);
    for (uint64_t p = 0; p < size; p += 4096) {
        IOByteCount segLen = 0;
        const addr64_t pa = md->getPhysicalSegment(off + p, &segLen, kIOMemoryMapperNone);
        uint64_t *pte = pa ? vmPte(c, va + p, true) : nullptr;
        if (!pte) {
            r = pa ? kIOReturnNoMemory : kIOReturnVMError;
            break;
        }
        *pte = (pa & kPtAddrMask) | kPteSysFlags;
        flushLine(pte);
    }
    if (c.hw)   // an unbound context is flushed when it is next bound
        vmFlushTlb(c.hw);
    IOLockUnlock(gpuLock);
    return r;
}

IOReturn PhxBringup::vmUnmap(uint32_t handle, uint64_t va, uint64_t size) {
    if (!vmCtx(handle))
        return kIOReturnNotReady;
    VmCtx &c = vm[handle - 1];
    IOLockLock(gpuLock);
    for (uint64_t p = 0; p < size; p += 4096) {
        uint64_t *pte = vmPte(c, va + p, false);
        if (pte) {
            *pte = 0;
            flushLine(pte);
        }
    }
    if (c.hw)
        vmFlushTlb(c.hw);
    IOLockUnlock(gpuLock);
    return kIOReturnSuccess;
}

void PhxBringup::ringPut(GpuQueue &q, uint32_t idx, uint32_t value) {
    const uint64_t slot = (q.wptr + idx) & (q.ringDw - 1);
    *reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.ringOff + slot * 4) = value;
}

uint64_t PhxBringup::queueFence(const GpuQueue &q) const {
    const volatile uint32_t *f = reinterpret_cast<const volatile uint32_t *>(fbAper->getVirtualAddress() + q.fenceOff);
    return static_cast<uint64_t>(f[0]) | (static_cast<uint64_t>(f[1]) << 32);
}

// Everything up to the returned sequence has finished: the oldest submission still running on any queue, minus one.
// Lock-free reader: an entry is written before `tail` moves past it.
uint64_t PhxBringup::gpuRetired() const {
    uint64_t r = gSeq;
    for (const GpuQueue &q : gq) {
        if (!q.ready)
            continue;
        const uint64_t done = queueFence(q);
        const uint32_t tail = q.tail;
        for (uint32_t i = q.head; i != tail; i++) {
            if (q.fseq[i & 63] > done) {
                if (q.fseq[i & 63] - 1 < r)
                    r = q.fseq[i & 63] - 1;
                break;
            }
        }
    }
    return r;
}

IOReturn PhxBringup::gpuSubmit(uint32_t handle, bool gfx, const uint64_t *ibVa, const uint32_t *ibDw, uint32_t n, uint64_t *seqOut) {
    // graphics IBs go to the gfx queue and compute IBs to the compute queue; with one of them missing, the other runs both
    GpuQueue *qp = gfx ? (gq[1].ready ? &gq[1] : &gq[0]) : (gq[0].ready ? &gq[0] : &gq[1]);
    if (!qp->ready || !vmCtx(handle))
        return kIOReturnNotReady;
    GpuQueue &q = *qp;
    const uint32_t need = n * 4 + 8;
    if (need + 16 > q.ringDw)
        return kIOReturnBadArgument;
    IOLockLock(gpuLock);
    // Ring space and the 64-entry history: retire finished entries, wait for the oldest one if needed (bounded, ~2 s).
    for (int spin = 0;; spin++) {
        const uint64_t done = queueFence(q);
        while (q.head != q.tail && q.fseq[q.head & 63] <= done) {
            q.doneEnd = q.fend[q.head & 63];
            q.head++;
        }
        if (q.tail - q.head < 63 && q.wptr + need - q.doneEnd <= q.ringDw - 16)
            break;
        if (spin > 40000) {
            IOLockUnlock(gpuLock);
            return kIOReturnBusy;
        }
        IODelay(50);
    }
    const uint32_t vmid = vmBind(handle);
    if (!vmid) {
        IOLockUnlock(gpuLock);
        return kIOReturnBusy;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        ringPut(q, k++, 0xC0023F00);                               // INDIRECT_BUFFER
        // the CP wants the canonical (sign-extended) address: an upper-half IB (bit 47, where RADV puts them) with a
        // bare 16-bit high word silently wedges the queue
        const uint64_t cva = (ibVa[i] & (1ull << 47)) ? (ibVa[i] | 0xFFFF000000000000ull) : ibVa[i];
        ringPut(q, k++, static_cast<uint32_t>(cva) & ~3u);
        ringPut(q, k++, static_cast<uint32_t>(cva >> 32));
        // size | VMID, plus VALID on compute (gfx_v11_0_ring_emit_ib_compute; the gfx variant has no VALID bit)
        ringPut(q, k++, ibDw[i] | (q.gfx ? 0u : (1u << 23)) | (vmid << 24));
    }
    const uint64_t seq = ++gSeq;
    hwv[vmid - kFirstUserVmid].lastSeq = seq;
    const uint64_t fenceMc = fbMcBase + q.fenceOff;
    ringPut(q, k++, 0xC0064900);                                   // RELEASE_MEM, as gfx_v11_0_ring_emit_fence
    ringPut(q, k++, 0x06603514);                                   // GCR SEQ|GL2_WB|GLM_INV|GLM_WB, CP 3, CACHE_FLUSH_AND_INV_TS, idx 5
    ringPut(q, k++, 0x40000000);                                   // DATA_SEL 2 (64-bit), no interrupt
    ringPut(q, k++, static_cast<uint32_t>(fenceMc));
    ringPut(q, k++, static_cast<uint32_t>(fenceMc >> 32));
    ringPut(q, k++, static_cast<uint32_t>(seq));
    ringPut(q, k++, static_cast<uint32_t>(seq >> 32));
    ringPut(q, k++, 0);
    q.wptr += k;
    q.fseq[q.tail & 63] = seq;
    q.fend[q.tail & 63] = q.wptr;
    q.fvmid[q.tail & 63] = vmid;
    __asm__ __volatile__("" ::: "memory");
    q.tail = q.tail + 1;
    // BAR0 is write-combined: drain the WC buffers so the packets are in VRAM before the CP is told about them
    __asm__ __volatile__("sfence" ::: "memory");
    (void)*reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.ringOff);
    hdpFlush();
    volatile uint32_t *wb = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + q.wbOff + 16);
    wb[0] = static_cast<uint32_t>(q.wptr);
    wb[1] = static_cast<uint32_t>(q.wptr >> 32);
    __asm__ __volatile__("sfence" ::: "memory");
    (void)wb[0];
    hdpFlush();
    __asm__ __volatile__("mfence" ::: "memory");
    *reinterpret_cast<volatile uint64_t *>(doorbellMap->getVirtualAddress() + q.dbByte) = q.wptr;
    IOLockUnlock(gpuLock);
    *seqOut = seq;
    return kIOReturnSuccess;
}

// Every 500 ms: a queue with unfinished work whose fence has not moved for debug.phx_hang_ms has the waves of its oldest
// unfinished submission killed, the CP then finishes that submission as if it had ended and the queue moves on.
void PhxBringup::hangTick(thread_call_param_t self, thread_call_param_t) {
    PhxBringup *me = static_cast<PhxBringup *>(self);
    me->hangCheck();
    if (me->hangStop)
        return;
    uint64_t d;
    clock_interval_to_deadline(500, kMillisecondScale, &d);
    thread_call_enter_delayed(me->hangCall, d);
}

void PhxBringup::hangCheck() {
    const int limitMs = gHangMs;
    if (limitMs <= 0 || !mmio)
        return;
    uint64_t limit, now = mach_absolute_time();
    nanoseconds_to_absolutetime(static_cast<uint64_t>(limitMs) * 1000000ull, &limit);
    for (GpuQueue &q : gq) {
        if (!q.ready)
            continue;
        const uint64_t done = queueFence(q);
        const uint32_t tail = q.tail;
        uint32_t oldest = 0;
        bool found = false;
        for (uint32_t i = q.head; i != tail && !found; i++)
            if (q.fseq[i & 63] > done) {
                oldest = i & 63;
                found = true;
            }
        if (!found || done != q.lastDone) {   // idle, or moving: the clock restarts
            q.lastDone = done;
            q.lastProgress = now;
            continue;
        }
        if (now - q.lastProgress < limit)
            continue;
        if (q.killedSeq != q.fseq[oldest]) {
            killWaves(q.fvmid[oldest], false);
            q.killedSeq = q.fseq[oldest];
            q.killedAll = false;
            q.lastProgress = now;
            IOLog("PhxBringup: %s queue stuck %d ms at seq %llu: killed the waves of VMID %u\n", q.gfx ? "gfx" : "compute",
                  limitMs, static_cast<unsigned long long>(q.fseq[oldest]), q.fvmid[oldest]);
        } else if (!q.killedAll && now - q.lastProgress >= limit / 2) {
            killWaves(0, true);
            q.killedAll = true;
            IOLog("PhxBringup: %s queue still stuck at seq %llu: killed every wave\n", q.gfx ? "gfx" : "compute",
                  static_cast<unsigned long long>(q.fseq[oldest]));
        }
    }
}

// gfx_v11_0_ring_soft_recovery: SQ_CMD kill (CMD 3) broadcast (MODE 1), limited to one VMID unless `all`, inside RLC
// safe mode
void PhxBringup::killWaves(uint32_t vmid, bool all) {
    rlcSafeMode(true);
    wreg(kRegSqCmd, 0x13u | (all ? 0u : (0x80u | (vmid & 0xfu) << 28)));
    rlcSafeMode(false);
    OSIncrementAtomic(&gHangKills);
}

// gfx_v11_0_set_safe_mode / gfx_v11_0_unset_safe_mode; rlcLock is held from enter to exit
void PhxBringup::rlcSafeMode(bool enter) const {
    if (enter) {
        if (rlcLock)
            IOLockLock(rlcLock);
        wreg(kRegRlcSafeMode, 0x3);                    // CMD | MESSAGE 1: enter
        for (int i = 0; i < 1000 && (rreg(kRegRlcSafeMode) & 1); i++)
            IODelay(1);
    } else {
        wreg(kRegRlcSafeMode, 0x1);                    // CMD, MESSAGE 0: exit
        if (rlcLock)
            IOLockUnlock(rlcLock);
    }
}

IOReturn PhxBringup::doorbellWrite(uint64_t byteOff, uint64_t value) {
    if (!doorbellMap)
        return kIOReturnNotReady;
    if ((byteOff & 7) || byteOff + 8 > doorbellMap->getLength())
        return kIOReturnBadArgument;
    *reinterpret_cast<volatile uint64_t *>(doorbellMap->getVirtualAddress() + byteOff) = value;
    return kIOReturnSuccess;
}

IOReturn PhxBringup::vramWrite(uint32_t byteOff, uint32_t value) {
    if (!fbAper)
        return kIOReturnNotReady;
    if ((byteOff & 3) || static_cast<uint64_t>(byteOff) + 4 > fbAperLen)
        return kIOReturnBadArgument;
    *reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + byteOff) = value;
    __asm__ __volatile__("sfence" ::: "memory");
    return kIOReturnSuccess;
}

// psp_wait_for(C2PMSG_64, MBOX_TOS_RESP_FLAG, MBOX_TOS_RESP_MASK): response flag set and status 0.
bool PhxBringup::pspWaitTos(uint32_t timeoutMs) const {
    for (uint32_t i = 0; i < timeoutMs * 10; i++) {
        if ((rreg(PHX_PSP_C2PMSG_64) & PHX_MBOX_TOS_MASK) == PHX_MBOX_TOS_READY)
            return true;
        IODelay(100);
    }
    return false;
}

// Debug selector 101: create / destroy the PSP 13.0.4 KM ring the way Linux psp_v13_0_ring_create / _stop and
// Cobalt64 psp_v14_0.cpp do. The ring lives in VRAM at carveOffset; the CPU writes it through the BAR0
// framebuffer aperture, which is MMIO above system RAM — so the PSP sees the bytes and the macOS heap is never
// touched. The MC address handed to the PSP is fbMcBase + carveOffset.
IOReturn PhxBringup::pspRing(uint32_t op, uint64_t carveOffset, uint64_t out[4]) {
    if (!mmio || !fbAper || !fbMcBase)
        return kIOReturnNotReady;
    IOReturn r = kIOReturnSuccess;
    IOLockLock(smuLock);
    if (op == 1) {
        if (ringMc) {
            r = kIOReturnExclusiveAccess;   // a ring is already live; destroy it first
        } else if ((carveOffset & 0xFFF) || carveOffset + PHX_PSP_FENCE_REL + 0x1000 > PHX_VRAM_TMR_OFF ||
                   PHX_VRAM_FWBUF_OFF + PHX_VRAM_FWBUF_SIZE > fbAperLen ||
                   (carveOffset <= PHX_VRAM_SMU_TABLE_OFF &&
                    PHX_VRAM_SMU_TABLE_OFF < carveOffset + PHX_PSP_FENCE_REL + 0x1000)) {
            // ring+cmd+fence must sit below the TMR and clear of the SMU table page; fw_buf must fit the aperture
            r = kIOReturnBadArgument;
        } else {
            volatile uint32_t *ring = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + carveOffset);
            uint32_t nonzero = 0;
            for (uint32_t i = 0; i < PHX_PSP_RING_BUF_SIZE / 4; i++)
                nonzero |= ring[i];
            out[2] = nonzero == 0;           // report whether the VRAM region was already clear (informational)
            for (uint32_t i = 0; i < PHX_PSP_RING_BUF_SIZE / 4; i++)
                ring[i] = 0;                 // zero the ring buffer (Linux memsets ring_mem; safe via BAR0)
            if (!pspWaitTos(2000)) {
                r = kIOReturnNotReady;       // SOS not ready to accept the ring
            } else {
                ringMc = fbMcBase + carveOffset;
                wreg(PHX_PSP_C2PMSG_69, static_cast<uint32_t>(ringMc));
                wreg(PHX_PSP_C2PMSG_70, static_cast<uint32_t>(ringMc >> 32));
                wreg(PHX_PSP_C2PMSG_71, PHX_PSP_RING_SIZE);
                wreg(PHX_PSP_C2PMSG_64, PHX_PSP_RING_TYPE_KM << 16);
                IOSleep(20);
                if (!pspWaitTos(2000)) {
                    r = kIOReturnTimeout;    // raw C2PMSG_64 (with PSP status in bits 15:0) returned in out[0]
                    ringMc = 0;              // not established: allow destroy + retry
                } else {
                    // Ring is live. Lay the GPCOM command + fence buffers just above it and zero them,
                    // then reset the doorbell so wptr agrees with our slot 0 (Linux psp_v13_0_ring_create).
                    ringOff = carveOffset;
                    cmdOff = carveOffset + PHX_PSP_CMD_REL;
                    fenceOff = carveOffset + PHX_PSP_FENCE_REL;
                    cmdMc = fbMcBase + cmdOff;
                    fenceMc = fbMcBase + fenceOff;
                    fenceCounter = 0;
                    tmrOff = PHX_VRAM_TMR_OFF;
                    tmrMc = fbMcBase + tmrOff;
                    tmrSize = 0;
                    tmrSetUp = false;
                    fwBufOff = PHX_VRAM_FWBUF_OFF;
                    fwBufMc = fbMcBase + fwBufOff;
                    fwBufSize = PHX_VRAM_FWBUF_SIZE;
                    volatile uint32_t *cmd = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + cmdOff);
                    volatile uint32_t *fen = reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + fenceOff);
                    for (uint32_t i = 0; i < 0x1000 / 4; i++) {
                        cmd[i] = 0;
                        fen[i] = 0;
                    }
                    wreg(PHX_PSP_C2PMSG_67, 0);
                }
            }
        }
    } else if (op == 2) {
        wreg(PHX_PSP_C2PMSG_64, PHX_GFX_CTRL_CMD_ID_DESTROY_RINGS);
        IOSleep(20);
        if (!pspWaitTos(2000))
            r = kIOReturnTimeout;
        clearRing();
    } else if (op != 3) {
        r = kIOReturnBadArgument;
    }
    out[0] = rreg(PHX_PSP_C2PMSG_64);
    out[1] = ringMc;
    out[3] = rreg(PHX_PSP_C2PMSG_67);
    IOLockUnlock(smuLock);
    return r;
}

// Core GPCOM submit (caller holds smuLock and a ring exists): write a prepared command — cmd_id plus up to
// nWords union dwords at +28 — into the VRAM command buffer, ring the doorbell, wait for the PSP's fence and
// read the response. Ports Linux psp_cmd_submit_buf / Cobalt64 psp_ring_cmd_submit.
IOReturn PhxBringup::pspRunCmd(uint32_t cmdId, const uint32_t *unionWords, uint32_t nWords, uint64_t out[4]) {
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress());
    volatile uint32_t *cmd = reinterpret_cast<volatile uint32_t *>(base + cmdOff);
    volatile uint32_t *fen = reinterpret_cast<volatile uint32_t *>(base + fenceOff);

    // Command buffer: zero it, set cmd_id (buf_size/buf_version stay 0, as upstream does), then the union.
    for (uint32_t i = 0; i < PHX_PSP_CMD_BUF_SIZE / 4; i++)
        cmd[i] = 0;
    cmd[PHX_PSP_CMD_HDR_CMD_ID_OFFSET / 4] = cmdId;
    for (uint32_t i = 0; i < nWords && i < 14; i++)
        cmd[PHX_PSP_GFX_CMD_UNION_OFFSET / 4 + i] = unionWords[i];

    uint32_t fence = ++fenceCounter;
    fen[0] = 0;

    // Ring frame at the slot the current wptr points to.
    uint32_t wptr = rreg(PHX_PSP_C2PMSG_67);
    const uint32_t ringSizeDw = PHX_PSP_RING_SIZE / 4;         // 1024
    const uint32_t frameSizeDw = PHX_PSP_GFX_FRAME_SIZE / 4;   // 16
    uint32_t slot = (wptr % ringSizeDw == 0) ? 0 : wptr / frameSizeDw;
    if (slot >= PHX_PSP_RING_SIZE / PHX_PSP_GFX_FRAME_SIZE)
        return kIOReturnInternalError;
    PhxGfxRbFrame frame;
    bzero(&frame, sizeof(frame));
    frame.cmd_buf_addr_lo = static_cast<uint32_t>(cmdMc);
    frame.cmd_buf_addr_hi = static_cast<uint32_t>(cmdMc >> 32);
    frame.fence_addr_lo = static_cast<uint32_t>(fenceMc);
    frame.fence_addr_hi = static_cast<uint32_t>(fenceMc >> 32);
    frame.fence_value = fence;
    volatile uint32_t *slotp =
        reinterpret_cast<volatile uint32_t *>(base + ringOff + static_cast<uint64_t>(slot) * PHX_PSP_GFX_FRAME_SIZE);
    const uint32_t *src = reinterpret_cast<const uint32_t *>(&frame);
    for (uint32_t i = 0; i < PHX_PSP_GFX_FRAME_SIZE / 4; i++)
        slotp[i] = src[i];

    // Drain CPU write buffers, flush the host data path, then ring the doorbell.
    __asm__ __volatile__("sfence" ::: "memory");
    hdpFlush();
    wptr = (wptr + frameSizeDw) % ringSizeDw;
    wreg(PHX_PSP_C2PMSG_67, wptr);

    // Wait for the PSP to write the fence value into VRAM (BAR0 read bypasses the CPU cache).
    uint32_t observed = 0, us = 0;
    const uint32_t budgetUs = 5000000;
    while (us < budgetUs) {
        observed = fen[0];
        if (observed == fence)
            break;
        IOSleep(1);
        us += 1000;
    }
    uint32_t status = cmd[PHX_PSP_GFX_RESP_OFFSET / 4];
    // Always succeed at the transport level so IOKit delivers the outputs; the PSP result is in out[].
    // out[1] = elapsed us (≈budget on a fence timeout), so userspace distinguishes timeout from a status code.
    out[0] = (observed == fence) ? status : 0xFFFFFFFFu;  // 0xFFFFFFFF = fence never arrived
    out[1] = us;
    out[2] = cmd[(PHX_PSP_GFX_RESP_OFFSET + 8) / 4];   // resp.fw_addr_lo / session_id
    out[3] = cmd[(PHX_PSP_GFX_RESP_OFFSET + 12) / 4];  // resp.fw_addr_hi
    return kIOReturnSuccess;
}

// Selector 103: no-payload query command (FB_FW_RESERV_ADDR/EXT only) — validates the ring without touching GPU power.
IOReturn PhxBringup::pspSubmit(uint32_t cmdId, uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    if (cmdId != PHX_PSP_GFX_CMD_FB_FW_RESERV_ADDR && cmdId != PHX_PSP_GFX_CMD_FB_FW_RESERV_EXT_ADDR)
        return kIOReturnNotPermitted;
    IOLockLock(smuLock);
    IOReturn r = ringMc ? pspRunCmd(cmdId, nullptr, 0, out) : kIOReturnNotReady;
    IOLockUnlock(smuLock);
    return r;
}

// Selector 104: stage a chunk of firmware bytes into the VRAM fw_buf through BAR0 (no submit).
IOReturn PhxBringup::fwStage(uint32_t dstOffset, const void *bytes, uint32_t len) {
    if (!fbAper || !fwBufSize)
        return kIOReturnNotReady;
    if ((dstOffset & 3) || static_cast<uint64_t>(dstOffset) + len > fwBufSize)
        return kIOReturnBadArgument;
    IOLockLock(smuLock);
    volatile uint8_t *dst = reinterpret_cast<volatile uint8_t *>(fbAper->getVirtualAddress() + fwBufOff + dstOffset);
    const uint8_t *s = static_cast<const uint8_t *>(bytes);
    uint32_t i = 0;
    for (; i + 4 <= len; i += 4) {
        uint32_t v;
        memcpy(&v, s + i, 4);
        *reinterpret_cast<volatile uint32_t *>(dst + i) = v;
    }
    if (i < len) {
        uint32_t v = 0;
        memcpy(&v, s + i, len - i);
        *reinterpret_cast<volatile uint32_t *>(dst + i) = v;
    }
    __asm__ __volatile__("sfence" ::: "memory");
    IOLockUnlock(smuLock);
    return kIOReturnSuccess;
}

// Selector 105: submit GFX_CMD_ID_LOAD_IP_FW for the firmware already staged in fw_buf at fwBufOffset.
IOReturn PhxBringup::loadIpFw(uint32_t fwType, uint32_t fwBufOffset, uint32_t size, uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    if ((fwBufOffset & 3) || size == 0 || static_cast<uint64_t>(fwBufOffset) + size > fwBufSize)
        return kIOReturnBadArgument;
    IOLockLock(smuLock);
    IOReturn r;
    if (!ringMc) {
        r = kIOReturnNotReady;
    } else {
        uint64_t mc = fwBufMc + fwBufOffset;
        uint32_t u[4] = {static_cast<uint32_t>(mc), static_cast<uint32_t>(mc >> 32), size, fwType};
        r = pspRunCmd(PHX_PSP_GFX_CMD_LOAD_IP_FW, u, 4, out);
    }
    IOLockUnlock(smuLock);
    return r;
}

// Selector 106: op 1 = SETUP_TMR (driver-managed TMR, Phoenix psp_13_0_4 is not in psp_skip_tmr), op 2 = AUTOLOAD_RLC.
IOReturn PhxBringup::pspCtl(uint32_t op, uint32_t arg, uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    IOReturn r;
    if (!ringMc) {
        r = kIOReturnNotReady;
    } else if (op == 1) {
        uint32_t size = arg ? arg : 0x400000u;
        if (size > PHX_VRAM_TMR_CAP)
            size = PHX_VRAM_TMR_CAP;
        tmrSize = size;
        // psp_gfx_cmd_setup_tmr (Linux psp_prep_tmr_cmd_buf): buf_phy_addr = TMR MC address, buf_size,
        // bitfield(virt_phy_addr=1 => 0x2), system_phy_addr = the REAL system-physical address of the TMR
        // (amdgpu_gmc_vram_pa = mc - vram_start + vram_base_offset = fbPhys + tmrOff on this UMA part).
        uint64_t tmrPa = fbPhys + tmrOff;
        uint32_t u[6] = {static_cast<uint32_t>(tmrMc), static_cast<uint32_t>(tmrMc >> 32), size, 0x2u,
                         static_cast<uint32_t>(tmrPa), static_cast<uint32_t>(tmrPa >> 32)};
        r = pspRunCmd(PHX_PSP_GFX_CMD_SETUP_TMR, u, 6, out);
        if (r == kIOReturnSuccess)
            tmrSetUp = true;
    } else if (op == 2) {
        r = pspRunCmd(PHX_PSP_GFX_CMD_AUTOLOAD_RLC, nullptr, 0, out);
    } else {
        r = kIOReturnBadArgument;
    }
    IOLockUnlock(smuLock);
    return r;
}

// Selector 107: submit an arbitrary GPCOM command with six caller-supplied union dwords. For bring-up
// experiments (SETUP_TMR / LOAD_TOC parameter sweeps) without rebuilding the kext.
IOReturn PhxBringup::pspRaw(uint32_t cmdId, const uint32_t words[6], uint64_t out[4]) {
    if (!mmio || !fbAper)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    IOReturn r = ringMc ? pspRunCmd(cmdId, words, 6, out) : kIOReturnNotReady;
    IOLockUnlock(smuLock);
    return r;
}

// Polls the SMU response register for up to 2 s (Linux: usec_timeout * 20); 0 means no answer.
uint32_t PhxBringup::smuPoll() const {
    for (uint32_t i = 0; i < 200000; i++) {
        uint32_t r = rreg(PHX_SMU_RESP_REG);
        if (r)
            return r;
        IODelay(10);
    }
    return 0;
}

// One SMU 13.0.4 mailbox transaction, the sequence of Linux smu_msg_v1_send_msg().
IOReturn PhxBringup::smuMessage(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *argAfter) {
    if (!mmio || !smuLock)
        return kIOReturnNotReady;
    IOLockLock(smuLock);
    // The first message of the boot skips the pre-poll (Linux SMU_FW_INIT): nothing may have been sent yet.
    if (smuUsed && smuPoll() == 0) {
        IOLockUnlock(smuLock);
        return kIOReturnBusy;
    }
    smuUsed = true;
    wreg(PHX_SMU_RESP_REG, 0);
    wreg(PHX_SMU_ARG_REG, arg);
    wreg(PHX_SMU_MSG_REG, msg);
    uint32_t r = smuPoll();
    *resp = r;
    *argAfter = rreg(PHX_SMU_ARG_REG);
    IOLockUnlock(smuLock);
    return r ? kIOReturnSuccess : kIOReturnTimeout;
}

// ---- power management (SMU 13.0.4) ----------------------------------------------------------------------------
// The sequence of Linux smu_v13_0_4_ppt.c / smu_v13_0.c: the driver table's MC address goes to the SMU once
// (SetDriverDramAddrHigh/Low), then TransferTableSmu2Dram(table id) has the PMFW write a table there. The table page is
// VRAM (the UMA carve-out); the CPU reads it through BAR0, like the PSP fence.

bool PhxBringup::pmSend(uint32_t msg, uint32_t arg) {
    uint32_t resp = 0, after = 0;
    if (smuMessage(msg, arg, &resp, &after) == kIOReturnSuccess && resp == PHX_PPSMC_Result_OK)
        return true;
    gPm.errors++;
    if (gPm.errors <= 8)
        IOLog("PhxBringup: pm: SMU msg 0x%x arg %u -> resp 0x%x\n", msg, arg, resp);
    return false;
}

IOReturn PhxBringup::pmTransfer(uint32_t tableId, void *out, uint32_t len) {
    if (!pmSend(PHX_PPSMC_TransferTableSmu2Dram, tableId))
        return kIOReturnError;
    volatile const uint32_t *src =
        reinterpret_cast<volatile const uint32_t *>(fbAper->getVirtualAddress() + PHX_VRAM_SMU_TABLE_OFF);
    uint32_t *dst = static_cast<uint32_t *>(out);
    for (uint32_t i = 0; i < len / 4; i++)
        dst[i] = src[i];
    return kIOReturnSuccess;
}

bool PhxBringup::pmSetup() {
    // SMU 13.0.4 only: Phoenix 0x15bf / 0x15c8 and this 780M (0x1900). Raphael (0x164e) speaks SMU 13.0.5.
    if (pciDeviceId != 0x15bf && pciDeviceId != 0x15c8 && pciDeviceId != 0x1900) {
        gPm.state = -1;
        pmDead = true;
        return false;
    }
    if (!mmio || !fbAper || !fbMcBase || PHX_VRAM_SMU_TABLE_OFF + 4096 > fbAperLen) {
        gPm.state = -2;
        pmDead = true;
        return false;
    }
    uint32_t ifv = 0, after = 0;
    if (smuMessage(PHX_PPSMC_GetDriverIfVersion, 0, &ifv, &after) != kIOReturnSuccess || ifv != PHX_PPSMC_Result_OK ||
        after != PHX_SMU13_0_4_DRIVER_IF_VERSION) {
        IOLog("PhxBringup: pm: DriverIf resp 0x%x version %u, want %u: off\n", ifv, after,
              PHX_SMU13_0_4_DRIVER_IF_VERSION);
        gPm.state = -1;
        pmDead = true;
        return false;
    }
    volatile uint32_t *page =
        reinterpret_cast<volatile uint32_t *>(fbAper->getVirtualAddress() + PHX_VRAM_SMU_TABLE_OFF);
    uint32_t nonzero = 0;
    for (uint32_t i = 0; i < 1024; i++)
        nonzero |= page[i];
    gPm.vramWasZero = nonzero == 0;
    for (uint32_t i = 0; i < 1024; i++)
        page[i] = 0;
    __asm__ __volatile__("sfence" ::: "memory");
    (void)page[0];
    hdpFlush();
    const uint64_t mc = fbMcBase + PHX_VRAM_SMU_TABLE_OFF;
    if (!pmSend(PHX_PPSMC_SetDriverDramAddrHigh, static_cast<uint32_t>(mc >> 32)) ||
        !pmSend(PHX_PPSMC_SetDriverDramAddrLow, static_cast<uint32_t>(mc))) {
        gPm.state = -3;
        if (++pmResets >= 5)
            pmDead = true;
        return false;
    }
    // the GFX range the PMFW allows (Linux smu_v13_0_4_set_fine_grain_gfx_freq_parameters)
    uint32_t dpm[PHX_SMU_DPMCLOCKS_SIZE / 4];
    if (pmTransfer(PHX_SMU_TABLE_DPMCLOCKS, dpm, sizeof(dpm)) == kIOReturnSuccess) {
        uint32_t lo = dpm[PHX_SMU_DPMCLOCKS_MINGFX_OFF / 4], hi = dpm[PHX_SMU_DPMCLOCKS_MINGFX_OFF / 4 + 1];
        if (lo >= 100 && hi <= 4000 && lo < hi) {
            gPm.dpmMin = static_cast<int>(lo);
            gPm.dpmMax = static_cast<int>(hi);
        }
    }
    IOLog("PhxBringup: pm: driver table at MC 0x%llx (page was %s), GFX DPM %d..%d MHz\n",
          (unsigned long long)mc, gPm.vramWasZero ? "clear" : "dirty", gPm.dpmMin, gPm.dpmMax);
    pmReady = true;
    pmFails = 0;
    gPm.state = 1;
    return true;
}

// HardMin <= SoftMax at every step: raising, the ceiling goes first; lowering, the floor.
bool PhxBringup::pmSetRange(int lo, int hi) {
    bool ok;
    if (lo > gPm.softMax && gPm.softMax)
        ok = pmSend(PHX_PPSMC_SetSoftMaxGfxClk, hi) && pmSend(PHX_PPSMC_SetHardMinGfxClk, lo);
    else
        ok = pmSend(PHX_PPSMC_SetHardMinGfxClk, lo) && pmSend(PHX_PPSMC_SetSoftMaxGfxClk, hi);
    if (ok) {
        gPm.hardMin = lo;
        gPm.softMax = hi;
    }
    return ok;
}

void PhxBringup::pmPolicy(const phx_smu13_0_4_metrics &m) {
    const int mode = gPmMode;
    // clocks only once the GFX block is powered and driven, and only inside the PMFW's own range
    if (!gpuReady() || !gPm.dpmMax)
        return;
    if (mode != 1 && mode != 2) {
        if (pmModeApplied && pmSetRange(gPm.dpmMin, gPm.dpmMax))
            pmModeApplied = 0;
        pmCap = 0;
        return;
    }
    const phxpm::Range r = phxpm::clampRange(0, gPmGfxMax, gPm.dpmMin, gPm.dpmMax);
    const int lo = r.lo, hi = r.hi;
    if (mode == 1) {
        // as Linux AUTO: set the range, re-assert it every ~10 s in case a GFX power-up reset it
        const uint32_t every = static_cast<uint32_t>(10000 / (gPmMs > 50 ? gPmMs : 50));
        if (pmModeApplied != 1 || gPm.hardMin != lo || gPm.softMax != hi || (every && pmTicks % every == 0))
            if (pmSetRange(lo, hi))
                pmModeApplied = 1;
        pmCap = 0;
        return;
    }
    // mode 2: an ondemand governor on the SoftMax (phx_smu_pm.h)
    const int want = phxpm::governorStep(pmCap, m.GfxActivity / 100, r, gPmUp, gPmTarget, gPmStepDown);
    if (pmModeApplied != 2 || gPm.hardMin != lo) {
        if (pmSetRange(lo, want)) {
            pmModeApplied = 2;
            pmCap = want;
        }
    } else if (want != gPm.softMax) {
        if (pmSend(PHX_PPSMC_SetSoftMaxGfxClk, static_cast<uint32_t>(want))) {   // floor unchanged: one message
            gPm.softMax = want;
            pmCap = want;
        }
    }
}

void PhxBringup::pmStep() {
    if (pmResync) {
        pmResync = false;
        pmReady = false;
        pmModeApplied = 0;
        pmCap = 0;
        gPm.hardMin = gPm.softMax = 0;
    }
    if (pmDead || (!pmReady && !pmSetup()))
        return;
    phx_smu13_0_4_metrics m;
    if (pmTransfer(PHX_SMU_TABLE_SMU_METRICS, &m, sizeof(m)) != kIOReturnSuccess) {
        // a busy PMFW answers 0xFC now and then; ten misses in a row: send the table address again, five such
        // resets: give up rather than keep the mailbox busy
        if (++pmFails >= 10) {
            pmReady = false;
            if (++pmResets >= 5) {
                IOLog("PhxBringup: pm: metrics table keeps failing, telemetry off\n");
                gPm.state = -4;
                pmDead = true;
            }
        }
        return;
    }
    pmFails = 0;
    pmTicks++;
    gPm.updates++;
    gPm.gfxclk = m.GfxclkFrequency;
    gPm.gfxclkAvg = m.AverageGfxclkFrequency;
    gPm.busy = m.GfxActivity / 100;
    gPm.busyAvg = m.AverageGfxActivity / 100;
    gPm.fclkAvg = m.AverageFclkFrequency;
    gPm.socclk = m.SocclkFrequency;
    gPm.memclk = m.MemclkFrequency;
    gPm.socketMw = m.CurrentSocketPower;
    gPm.socketMwAvg = m.AverageSocketPower;
    gPm.coreMwAvg = m.AverageCorePower;
    gPm.apuMw = static_cast<int>(m.ApuPower);
    gPm.vddMw = m.Power[0];
    gPm.socMw = m.Power[1];
    gPm.gfxTemp = m.GfxTemperature;
    gPm.socTemp = m.SocTemperature;
    gPm.throttler = m.ThrottlerStatus;
    uint32_t c0 = 0;
    for (uint16_t r : m.AverageCoreC0Residency)
        c0 += r;
    gPm.c0 = static_cast<int>(c0 / 8);
    pmPolicy(m);
    cgPolicy();
}

// RLC_CGTT_MGCG_OVERRIDE, RLC_CGCG_CGLS_CTRL, RLC_CGCG_CGLS_CTRL_3D, CP_RB_WPTR_POLL_CNTL, CP_INT_CNTL_RING0,
// SDMA0_RLC_CGCG_CTRL (dwords, GC 11.0.1 / SDMA 6.0.1)
static constexpr uint32_t kCgRegs[6] = {0xEC48, 0xEC49, 0xECC5, 0x21C2, 0x306A, 0x12BC};
static constexpr uint32_t kCgMgcgOverrideOn = 0x000600c1;   // what Linux leaves: MGCG/CGCG/CGLS overrides clear
static constexpr uint32_t kCgCgcgCglsCtrlOn = 0x0000363f;   // CGCG_EN | CGLS_EN, Linux's idle threshold and CGLS rep

// From the PM tick: GFX clock gating on/off as debug.phx_pm_cg asks.
void PhxBringup::cgPolicy() {
    if (!mmio || !gpuReady())
        return;
    const bool want = gPmCg == 1;
    if (want && cgOn && (++cgChecks & 15))
        return;                                   // on: look at the override register every 16th tick
    if (want) {
        if (cgOn && rreg(kCgRegs[0]) == kCgMgcgOverrideOn)
            return;
        if (!cgSaved) {
            for (int i = 0; i < 6; i++)
                cgOld[i] = rreg(kCgRegs[i]);
            cgSaved = true;
        }
        rlcSafeMode(true);
        wreg(kCgRegs[0], kCgMgcgOverrideOn);
        wreg(kCgRegs[1], kCgCgcgCglsCtrlOn);
        wreg(kCgRegs[2], kCgCgcgCglsCtrlOn);
        wreg(kCgRegs[3], 0x00900100);             // CP_RB_WPTR_POLL_CNTL: poll frequency 0x100, idle poll count 0x90
        wreg(kCgRegs[4], rreg(kCgRegs[4]) | 0x003c0000);   // CNTX_BUSY/EMPTY, CMP_BUSY, GFX_IDLE interrupt enables
        wreg(kCgRegs[5], rreg(kCgRegs[5]) | 0x2);           // SDMA0 CGCG enable
        rlcSafeMode(false);
        cgOn = true;
        gPm.cgState = 1;
        gPm.cgApplies++;
        IOLog("PhxBringup: clock gating on (MGCG override 0x%x -> 0x%x)\n", cgOld[0], rreg(kCgRegs[0]));
    } else if (cgOn) {
        rlcSafeMode(true);
        for (int i = 0; i < 6; i++)
            wreg(kCgRegs[i], cgOld[i]);
        rlcSafeMode(false);
        cgOn = false;
        gPm.cgState = 0;
        IOLog("PhxBringup: clock gating off\n");
    }
}

void PhxBringup::pmTick(thread_call_param_t self, thread_call_param_t) {
    PhxBringup *me = static_cast<PhxBringup *>(self);
    int ms = gPmMs;
    if (ms > 0 && !me->isSleeping)
        me->pmStep();
    if (me->pmStop || me->pmDead)
        return;
    if (ms <= 0)
        ms = 1000;          // off: look again in a second
    else if (ms < 50)
        ms = 50;
    uint64_t d;
    clock_interval_to_deadline(static_cast<uint32_t>(ms), kMillisecondScale, &d);
    thread_call_enter_delayed(me->pmCall, d);
}

bool PhxBringup::claimSession() { return OSCompareAndSwap(0, 1, &sessionOpen); }

void PhxBringup::releaseSession() { OSCompareAndSwap(1, 0, &sessionOpen); }

IOReturn PhxBringup::readRegs(uint32_t first, uint32_t count, uint32_t *out) {
    if (!mmio)
        return kIOReturnNotReady;
    uint64_t len = mmio->getLength();
    if ((static_cast<uint64_t>(first) + count) * 4 > len)
        return kIOReturnBadArgument;
    volatile uint32_t *r = reinterpret_cast<volatile uint32_t *>(mmio->getVirtualAddress());
    for (uint32_t i = 0; i < count; i++)
        out[i] = r[first + i];
    return kIOReturnSuccess;
}

IOReturn PhxBringup::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) {
    // the sysctls go up with the first client, from user space, rather than in start(): the bootloader injects this
    // kext into the boot collection, and it starts long before the rest of BSD
    if (!gSysctlUp && OSCompareAndSwap(0, 1, &gSysctlUp)) {
        sysctl_register_oid(&sysctl__debug_phxbos_flush);
        sysctl_register_oid(&sysctl__debug_phxbos_flush_bytes);
        sysctl_register_oid(&sysctl__debug_phx_hang_ms);
        sysctl_register_oid(&sysctl__debug_phx_hang_kills);
        for (struct sysctl_oid *o : kPmOids)
            sysctl_register_oid(o);
    }
    if (type != PHXN_UC_TYPE)
        return kIOReturnBadArgument;
    IOAccelPhxNativeClient *client = OSTypeAlloc(IOAccelPhxNativeClient);
    if (!client)
        return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type) || !client->attach(this)) {
        client->release();
        return kIOReturnNoMemory;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnExclusiveAccess;
    }
    *handler = client;
    return kIOReturnSuccess;
}

// ---- user client ----------------------------------------------------------------------------------------

bool IOAccelPhxNativeClient::initWithTask(task_t owningTask, void *securityID, UInt32 type) {
    if (!IOUserClient::initWithTask(owningTask, securityID, type))
        return false;
    privileged = clientHasPrivilege(owningTask, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    task = owningTask;
    lock = IOLockAlloc();
    bos = static_cast<Bo *>(IOMallocZero(sizeof(Bo) * PHXN_MAX_BOS));
    maps = static_cast<Map *>(IOMallocZero(sizeof(Map) * kMaxMaps));
    return lock && bos && maps;
}

bool IOAccelPhxNativeClient::start(IOService *provider) {
    owner = OSDynamicCast(PhxBringup, provider);
    if (!owner)
        return false;
    // E9: several processes at once, each in its own GPUVM context (vmidAlloc); the first one still marks the session
    if (owner->claimSession())
        sessionClaimed = true;
    return IOUserClient::start(provider);
}

void IOAccelPhxNativeClient::freeAll() {
    if (!bos)
        return;
    for (uint32_t h = 1; h < PHXN_MAX_BOS; h++)
        if (bos[h].md)
            boRelease(bos[h]);
}

IOReturn IOAccelPhxNativeClient::clientClose() {
    if (lock)
        IOLockLock(lock);
    freeAll();
    if (lock)
        IOLockUnlock(lock);
    if (vmid && owner) {
        owner->vmidFree(vmid, lastSeq);
        vmid = 0;
    }
    if (sessionClaimed && owner) {
        owner->releaseSession();
        sessionClaimed = false;
    }
    terminate();
    return kIOReturnSuccess;
}

void IOAccelPhxNativeClient::free() {
    if (vmid && owner) {
        owner->vmidFree(vmid, lastSeq);
        vmid = 0;
    }
    freeAll();
    if (sessionClaimed && owner)
        owner->releaseSession();
    if (bos)
        IOFree(bos, sizeof(Bo) * PHXN_MAX_BOS);
    if (maps)
        IOFree(maps, sizeof(Map) * kMaxMaps);
    if (lock)
        IOLockFree(lock);
    IOUserClient::free();
}

IOAccelPhxNativeClient::Bo *IOAccelPhxNativeClient::findBo(uint32_t h) {
    if (h == 0 || h >= PHXN_MAX_BOS || !bos[h].md)
        return nullptr;
    return &bos[h];
}

IOReturn IOAccelPhxNativeClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
    IOLockLock(lock);
    Bo *b = findBo(type);
    if (!b) {
        IOLockUnlock(lock);
        return kIOReturnNotFound;
    }
    b->md->retain();
    *memory = b->md;
    *options = 0;
    IOLockUnlock(lock);
    return kIOReturnSuccess;
}

// Exact shape check, as in the contract: scalar counts and struct sizes must match.
static bool shape(const IOExternalMethodArguments *a, uint32_t sin, uint32_t sout, uint32_t stin, uint32_t stout) {
    return a->scalarInputCount == sin && a->scalarOutputCount == sout && a->structureInputSize == stin &&
           a->structureOutputSize == stout && !a->structureInputDescriptor && !a->structureOutputDescriptor;
}

IOReturn IOAccelPhxNativeClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                                IOExternalMethodDispatch *, OSObject *, void *) {
    if (selector == PHXN_SEL_WAITSEQ)
        return doWaitSeq(a);
    if (selector == PHXN_SEL_TOPOLOGY)
        return doTopology(a);
    if (selector == PHXN_SEL_SURF_GPU_MARK)   // no client state: a ready mark must not wait behind a submission
        return doSurfGpuMark(a);

    IOLockLock(lock);
    IOReturn r;
    if (selector != PHXN_SEL_HELLO && !hello) {
        r = kIOReturnNotReady;
    } else {
        switch (selector) {
        case PHXN_SEL_HELLO:     r = doHello(a); break;
        case PHXN_SEL_QUERYINFO: r = doQueryInfo(a); break;
        case PHXN_SEL_READREGS:  r = doReadRegs(a); break;
        case PHXN_SEL_BOCREATE:  r = doBoCreate(a); break;
        case PHXN_SEL_BOFREE:    r = doBoFree(a); break;
        case PHXN_SEL_BOIMPORTHOST: r = doBoImportHost(a); break;
        case PHXN_SEL_GEMVA:     r = doGemVa(a); break;
        case PHXN_SEL_CTX:       r = doCtx(a); break;
        case PHXN_SEL_SUBMIT:    r = doSubmit(a); break;
        case PHXN_SEL_DBG_SMU:   r = doDbgSmu(a); break;
        case PHXN_SEL_DBG_PSP_RING: r = doDbgPspRing(a); break;
        case PHXN_SEL_DBG_VRAM_READ: r = doDbgVramRead(a); break;
        case PHXN_SEL_DBG_PSP_SUBMIT: r = doDbgPspSubmit(a); break;
        case PHXN_SEL_DBG_FW_STAGE: r = doDbgFwStage(a); break;
        case PHXN_SEL_DBG_PSP_LOADIPFW: r = doDbgLoadIpFw(a); break;
        case PHXN_SEL_DBG_PSP_CTL: r = doDbgPspCtl(a); break;
        case PHXN_SEL_DBG_PSP_RAW: r = doDbgPspRaw(a); break;
        case PHXN_SEL_DBG_WRITE_REG: r = doDbgWriteReg(a); break;
        case PHXN_SEL_DBG_VRAM_WRITE: r = doDbgVramWrite(a); break;
        case PHXN_SEL_DBG_DOORBELL: r = doDbgDoorbell(a); break;
        case PHXN_SEL_DBG_SYSMEM: r = doDbgSysMem(a); break;
        case PHXN_SEL_DBG_ADOPTQ: r = doDbgAdoptQueue(a); break;
        case PHXN_SEL_DBG_CLAIM: r = doDbgClaim(a); break;
        default:                 r = kIOReturnBadArgument; break;
        }
    }
    IOLockUnlock(lock);
    return r;
}

IOReturn IOAccelPhxNativeClient::doHello(IOExternalMethodArguments *a) {
    if (!shape(a, 2, 4, 0, 0))
        return kIOReturnBadArgument;
    if (a->scalarInput[1] != 0 && a->scalarInput[1] != PHXN_HELLO_F_MINOR)
        return kIOReturnBadArgument;
    if (a->scalarInput[0] != PHXN_ABI_VERSION)
        return kIOReturnUnsupported;
    hello = true;
    a->scalarOutput[0] = PHXN_ABI_VERSION | (a->scalarInput[1] ? static_cast<uint64_t>(PHXN_ABI_MINOR) << 16 : 0);
    a->scalarOutput[1] = PHX_KEXT_BUILD;
    a->scalarOutput[2] = PHXN_MAX_IBS;
    a->scalarOutput[3] = PHXN_VMID;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doQueryInfo(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, 0, sizeof(phxn_info)))
        return kIOReturnBadArgument;
    phxn_info inf;
    bzero(&inf, sizeof(inf));
    inf.abi_version = PHXN_ABI_VERSION;
    inf.kext_build = PHX_KEXT_BUILD;
    inf.reserved[2] = PHXN_ABI_MINOR;
    inf.flags = PHXN_INFO_S1B_POSITIVE | PHXN_INFO_NATIVE_BOOT;
    inf.vmid = PHXN_VMID;
    inf.gb_addr_config = kGbAddrConfig;
    inf.gc_version = kGcVersion;
    inf.ring_size_dw = 4096;
    inf.max_ibs = PHXN_MAX_IBS;
    inf.vram_vis_total = kVisTotal;
    inf.vram_vis_free = kVisTotal - visUsed;
    inf.vram_hi_total = kHiTotal;
    inf.vram_hi_free = kHiTotal - hiUsed;
    inf.gtt_cap = kGttCap;
    inf.gtt_used = gttUsed;
    inf.gtt_max_bo = kGttMaxBo;
    inf.seq_emitted = emitted;
    inf.seq_retired = owner->gpuReady() ? owner->gpuRetired() : emitted;
    inf.va_low_first = 0x10000;
    inf.va_low_last = 0x7FFFFFFFFFFFull;
    inf.va_high_first = 0xFFFF800000000000ull;
    inf.va_high_last = 0xFFFFFFFFFFFFFFFFull;
    inf.max_bos = PHXN_MAX_BOS;
    inf.fence_slots = PHXN_FENCE_SLOTS;
    bcopy(&inf, a->structureOutput, sizeof(inf));
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doTopology(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, 0, sizeof(phx_topology)))
        return kIOReturnBadArgument;
    const phx_topology *p = owner->capturedTopology();
    if (!p)
        return kIOReturnUnsupported;
    bcopy(p, a->structureOutput, sizeof(*p));
    return kIOReturnSuccess;
}

static constexpr uint64_t kGoldenTscUpper = 0x16a2d;
IOReturn IOAccelPhxNativeClient::doReadRegs(IOExternalMethodArguments *a) {
    if (a->scalarInputCount != 3 || a->scalarOutputCount != 0 || a->structureInputSize != 0)
        return kIOReturnBadArgument;
    uint64_t count = a->scalarInput[1];
    if (count < 1 || count > 16 || a->scalarInput[2] != 0xFFFFFFFFull || a->structureOutputSize != count * 4 ||
        a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    // SMUIO's GOLDEN_TSC (upper 0x16a2d, lower 0x16a2e on Phoenix, SMUIO 13.0.7 base 0x16a00) is the 100 MHz counter the
    // CP stamps command buffers with: every client may read it (Mesa's AMDGPU_INFO_TIMESTAMP, which calibrates GPU
    // times against the host's; without it Metal's GPUStartTime/GPUEndTime were 0 in every app).
    const uint64_t first = a->scalarInput[0];
    const bool tsc = first >= kGoldenTscUpper && first + count <= kGoldenTscUpper + 2;
    if (!privileged && !tsc)
        return kIOReturnNotPrivileged;
    return owner->readRegs(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(count),
                           static_cast<uint32_t *>(a->structureOutput));
}

IOReturn IOAccelPhxNativeClient::doBoCreate(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 4, sizeof(phxn_gem_create_in), 0))
        return kIOReturnBadArgument;
    const phxn_gem_create_in *in = static_cast<const phxn_gem_create_in *>(a->structureInput);
    uint64_t size = in->bo_size;
    if (size == 0)
        return kIOReturnBadArgument;
    size = (size + 4095) & ~4095ull;
    uint64_t align = in->alignment;
    if (align && ((align & (align - 1)) || align > (2ull << 20)))
        return kIOReturnBadArgument;
    if (align < 4096)
        align = 4096;
    const uint64_t d = in->domains, f = in->domain_flags;
    if (d == 0 || (d & ~static_cast<uint64_t>(PHX_AMDGPU_GEM_DOMAIN_GTT | PHX_AMDGPU_GEM_DOMAIN_VRAM)))
        return kIOReturnUnsupported;
    if (f & (PHX_AMDGPU_GEM_CREATE_ENCRYPTED | PHX_AMDGPU_GEM_CREATE_CP_MQD_GFX9 | PHX_AMDGPU_GEM_CREATE_PREEMPTIBLE))
        return kIOReturnUnsupported;

    uint32_t domain, bits;
    uint64_t *pool;
    if (d & PHX_AMDGPU_GEM_DOMAIN_VRAM) {
        if (visUsed + size <= kVisTotal) {
            domain = 4;
            bits = PHXN_PLACED_CPU_MAPPABLE | PHXN_PLACED_ZEROED;
            pool = &visUsed;
        } else if ((f & PHX_AMDGPU_GEM_CREATE_NO_CPU_ACCESS) && !(f & PHX_AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) &&
                   hiUsed + size <= kHiTotal) {
            domain = 4;
            bits = PHXN_PLACED_HI_POOL | PHXN_PLACED_ZEROED;
            pool = &hiUsed;
        } else {
            return kIOReturnNoMemory;
        }
    } else {
        if (size > kGttMaxBo || gttUsed + size > kGttCap)
            return kIOReturnNoMemory;
        domain = 2;
        bits = PHXN_PLACED_ZEROED;
        pool = &gttUsed;
    }

    uint32_t h = 1;
    while (h < PHXN_MAX_BOS && bos[h].md)
        h++;
    if (h >= PHXN_MAX_BOS)
        return kIOReturnNoResources;

    // Page alignment only: the GPU maps BOs page by page, and the client picks the VA (GEMVA) with its own alignment.
    // Handed to IOBufferMemoryDescriptor, RADV's 64 KiB..2 MiB alignment made every allocation PHYSICALLY CONTIGUOUS
    // (XNU allocates an alignment above the page size with kmem_alloc_contig): once memory was fragmented, every
    // buffer of 8 MiB or more failed after a 400 ms search, and each Metal app spent ~0.8 s at launch (2026-10-21).
    (void)align;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, size, PAGE_SIZE);
    if (!md)
        return kIOReturnNoMemory;
    bzero(md->getBytesNoCopy(), size);
    *pool += size;
    bos[h] = Bo{md, size, domain, bits, static_cast<uint8_t *>(md->getBytesNoCopy()), false};
    a->scalarOutput[0] = h;
    a->scalarOutput[1] = size;
    a->scalarOutput[2] = domain;
    a->scalarOutput[3] = bits;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doBoFree(IOExternalMethodArguments *a) {
    if (!shape(a, 1, 0, 0, 0))
        return kIOReturnBadArgument;
    uint32_t h = static_cast<uint32_t>(a->scalarInput[0]);
    Bo *b = findBo(h);
    if (!b)
        return kIOReturnNotFound;
    for (int i = 0; i < kMaxMaps; i++)
        if (maps[i].used && maps[i].handle == h)
            maps[i].used = false;
    if (b->host)
        hostUsed -= b->size;
    else if (b->domain == 2)
        gttUsed -= b->size;
    else if (b->bits & PHXN_PLACED_HI_POOL)
        hiUsed -= b->size;
    else
        visUsed -= b->size;
    boRelease(*b);
    return kIOReturnSuccess;
}

void IOAccelPhxNativeClient::boRelease(Bo &b) {
    if (b.host)
        b.md->complete();
    b.md->release();
    b = Bo{};
}

// PHXN_SEL_BOIMPORTHOST (ABI 1.9, VK_EXT_external_memory_host): a BO over the caller's own pages, wired for as long as
// it lives, GPU-mapped later through GemVa like any other. The kext reported ABI 1.12 without it, so every import
// RADV made (no-copy buffers, IOSurface textures over their surfaces' pages) failed and fell back to copies
// (2026-10-20). in: [0] host VA [1] size (both page-aligned) [2] flags 0 [3] GPU VA 0; out: [0] handle [1] size
// [2] GPU VA 0 [3] placement bits.
IOReturn IOAccelPhxNativeClient::doBoImportHost(IOExternalMethodArguments *a) {
    if (!shape(a, 4, 4, 0, 0))
        return kIOReturnBadArgument;
    const uint64_t va = a->scalarInput[0], size = a->scalarInput[1];
    if (!va || !size || (va & 4095) || (size & 4095) || a->scalarInput[2] || a->scalarInput[3] || !task)
        return kIOReturnBadArgument;
    if (size > kHostMaxBo || hostUsed + size > kHostCap)
        return kIOReturnNoMemory;
    uint32_t h = 1;
    while (h < PHXN_MAX_BOS && bos[h].md)
        h++;
    if (h >= PHXN_MAX_BOS)
        return kIOReturnNoResources;
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(va, size, kIODirectionInOut, task);
    if (!md)
        return kIOReturnNoMemory;
    const IOReturn r = md->prepare();
    if (r != kIOReturnSuccess) {
        md->release();
        return r;
    }
    hostUsed += size;
    bos[h] = Bo{md, size, 2, 0, nullptr, true};
    a->scalarOutput[0] = h;
    a->scalarOutput[1] = size;
    a->scalarOutput[2] = 0;
    a->scalarOutput[3] = 0;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doGemVa(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, sizeof(phxn_gem_va), 0))
        return kIOReturnBadArgument;
    const phxn_gem_va *v = static_cast<const phxn_gem_va *>(a->structureInput);
    if (v->_pad || v->vm_timeline_point || v->vm_timeline_syncobj_out || v->num_syncobj_handles ||
        v->input_fence_syncobj_handles)
        return kIOReturnBadArgument;
    if (v->handle == 0 || (v->flags & PHX_AMDGPU_VM_PAGE_PRT))
        return kIOReturnUnsupported;
    if (v->operation == PHX_AMDGPU_VA_OP_CLEAR || v->operation == PHX_AMDGPU_VA_OP_REPLACE)
        return kIOReturnUnsupported;
    if (v->operation != PHX_AMDGPU_VA_OP_MAP && v->operation != PHX_AMDGPU_VA_OP_UNMAP)
        return kIOReturnBadArgument;
    Bo *b = findBo(v->handle);
    if (!b)
        return kIOReturnNotFound;
    if (!vaCanonical(v->va_address))
        return kIOReturnBadArgument;
    const uint64_t va = v->va_address & kVaMask;
    if (((v->va_address | v->offset_in_bo | v->map_size) & 4095) || v->map_size == 0)
        return kIOReturnBadArgument;
    if (va < 0x10000 || va + v->map_size > (1ull << 48))
        return kIOReturnBadArgument;
    if (v->offset_in_bo + v->map_size > b->size)
        return kIOReturnBadArgument;

    if (v->operation == PHX_AMDGPU_VA_OP_MAP) {
        int freei = -1;
        for (int i = 0; i < kMaxMaps; i++) {
            if (!maps[i].used) {
                if (freei < 0)
                    freei = i;
                continue;
            }
            if (va < maps[i].va + maps[i].size && maps[i].va < va + v->map_size)
                return kIOReturnBadArgument;
        }
        if (freei < 0)
            return kIOReturnNoMemory;
        maps[freei] = Map{true, v->handle, va, v->map_size, v->offset_in_bo, v->flags};
        if (owner->gpuReady()) {
            const IOReturn r = ensureVmid() ? owner->vmMap(vmid, va, b->md, v->offset_in_bo, v->map_size) : kIOReturnNoResources;
            if (r != kIOReturnSuccess) {
                maps[freei].used = false;
                return r;
            }
        }
        return kIOReturnSuccess;
    }
    for (int i = 0; i < kMaxMaps; i++) {
        if (maps[i].used && maps[i].handle == v->handle && maps[i].va == va && maps[i].size == v->map_size) {
            maps[i].used = false;
            if (vmid)
                owner->vmUnmap(vmid, va, v->map_size);
            return kIOReturnSuccess;
        }
    }
    return kIOReturnBadArgument;
}

IOReturn IOAccelPhxNativeClient::doCtx(IOExternalMethodArguments *a) {
    if (!shape(a, 0, 0, sizeof(phxn_ctx), sizeof(phxn_ctx)))
        return kIOReturnBadArgument;
    phxn_ctx in, out;
    bcopy(a->structureInput, &in, sizeof(in));
    bzero(&out, sizeof(out));
    if (in.op == PHX_AMDGPU_CTX_OP_ALLOC_CTX) {
        uint32_t id = 1;
        while (id <= PHXN_MAX_CTX && ctx[id])
            id++;
        if (id > PHXN_MAX_CTX)
            return kIOReturnNoResources;
        ctx[id] = true;
        out.op = id;
    } else if (in.op == PHX_AMDGPU_CTX_OP_FREE_CTX) {
        if (in.ctx_id == 0 || in.ctx_id > PHXN_MAX_CTX || !ctx[in.ctx_id])
            return kIOReturnNotFound;
        ctx[in.ctx_id] = false;
    } else if (in.op != PHX_AMDGPU_CTX_OP_QUERY_STATE2) {
        return kIOReturnBadArgument;
    }
    bcopy(&out, a->structureOutput, sizeof(out));
    return kIOReturnSuccess;
}

bool IOAccelPhxNativeClient::ibCovered(uint64_t vaCanon, uint64_t bytes) {
    uint64_t cur = vaCanon & kVaMask;
    const uint64_t end = cur + bytes;
    while (cur < end) {
        bool ok = false;
        for (int i = 0; i < kMaxMaps; i++) {
            const Map &mp = maps[i];
            if (mp.used && (mp.flags & PHX_AMDGPU_VM_PAGE_EXECUTABLE) && cur >= mp.va && cur < mp.va + mp.size) {
                cur = mp.va + mp.size < end ? mp.va + mp.size : end;
                ok = true;
                break;
            }
        }
        if (!ok)
            return false;
    }
    return true;
}

IOReturn IOAccelPhxNativeClient::doSubmit(IOExternalMethodArguments *a) {
    if (a->scalarInputCount != 0 || a->scalarOutputCount != 1 || a->structureOutputSize != 0 ||
        a->structureInputDescriptor || a->structureInputSize < 64 ||
        a->structureInputSize > 32 + 32 * PHXN_MAX_IBS)
        return kIOReturnBadArgument;
    const uint8_t *in = static_cast<const uint8_t *>(a->structureInput);
    phxn_cs_in h;
    bcopy(in, &h, sizeof(h));
    if (h.abi != PHXN_ABI_VERSION || h.num_ibs < 1 || h.num_ibs > PHXN_MAX_IBS ||
        a->structureInputSize != sizeof(h) + h.num_ibs * sizeof(phxn_cs_ib) || h.reserved != 0 ||
        (h.flags & ~PHXN_CS_HAS_FENCE))
        return kIOReturnBadArgument;
    if (h.ctx_id == 0 || h.ctx_id > PHXN_MAX_CTX || !ctx[h.ctx_id])
        return kIOReturnNotFound;

    for (uint32_t i = 0; i < h.num_ibs; i++) {
        phxn_cs_ib ib;
        bcopy(in + sizeof(h) + i * sizeof(ib), &ib, sizeof(ib));
        if (ib._pad || (ib.ip_type != PHX_AMDGPU_HW_IP_GFX && ib.ip_type != PHX_AMDGPU_HW_IP_COMPUTE) || ib.ip_instance != 0 || ib.ring != 0)
            return kIOReturnBadArgument;
        if (ib.ib_bytes == 0 || (ib.ib_bytes & 3) || ib.ib_bytes > 0xFFFFFu * 4)
            return kIOReturnBadArgument;
        if (!vaCanonical(ib.va_start) || (ib.va_start & 3))
            return kIOReturnBadArgument;
        if (ib.flags & (PHX_AMDGPU_IB_FLAG_CE | PHX_AMDGPU_IB_FLAG_RESET_GDS_MAX_WAVE_ID | PHX_AMDGPU_IB_FLAGS_SECURE |
                        PHX_AMDGPU_IB_FLAG_EMIT_MEM_SYNC))
            return kIOReturnUnsupported;
        if (ib.flags & ~static_cast<uint32_t>(PHX_AMDGPU_IB_FLAG_PREAMBLE | PHX_AMDGPU_IB_FLAG_PREEMPT |
                                              PHX_AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE))
            return kIOReturnBadArgument;
        if (!ibCovered(ib.va_start, ib.ib_bytes))
            return kIOReturnBadArgument;
    }

    Bo *fb = nullptr;
    if (h.flags & PHXN_CS_HAS_FENCE) {
        fb = findBo(h.fence_handle);
        if (!fb)
            return kIOReturnNotFound;
        if (!fb->kva || (h.fence_offset & 7) || static_cast<uint64_t>(h.fence_offset) + 8 > fb->size)
            return kIOReturnBadArgument;   // the kernel writes the fence: not into a host import
    } else if (h.fence_handle || h.fence_offset) {
        return kIOReturnBadArgument;
    }

    if (owner->gpuReady()) {
        // E8: real execution on the adopted compute queue, IBs run in VMID 8
        uint64_t va[PHXN_MAX_IBS];
        uint32_t dw[PHXN_MAX_IBS];
        for (uint32_t i = 0; i < h.num_ibs; i++) {
            phxn_cs_ib ib;
            bcopy(in + sizeof(h) + i * sizeof(ib), &ib, sizeof(ib));
            va[i] = ib.va_start & kVaMask;
            dw[i] = ib.ib_bytes / 4;
        }
        // log the IBs RADV sends (first 96 dwords each) for bring-up: a client's first submissions only, as every
        // WindowServer frame dumped its IBs and the log held nothing else (2026-10-20)
        if (owner->debugEnabled() && emitted < 4) {
            for (uint32_t i = 0; i < h.num_ibs; i++) {
                for (int m = 0; m < kMaxMaps; m++) {
                    if (!maps[m].used || va[i] < maps[m].va || va[i] >= maps[m].va + maps[m].size)
                        continue;
                    Bo *bo = findBo(maps[m].handle);
                    if (!bo || !bo->kva)
                        break;
                    const uint32_t *d = reinterpret_cast<const uint32_t *>(bo->kva + maps[m].off + (va[i] - maps[m].va));
                    const uint32_t cnt = dw[i] < 96 ? dw[i] : 96;
                    for (uint32_t j = 0; j < cnt; j += 8)
                        IOLog("PhxIB %u.%u +%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned)(emitted + 1), i, j,
                              d[j], j + 1 < cnt ? d[j + 1] : 0, j + 2 < cnt ? d[j + 2] : 0, j + 3 < cnt ? d[j + 3] : 0,
                              j + 4 < cnt ? d[j + 4] : 0, j + 5 < cnt ? d[j + 5] : 0, j + 6 < cnt ? d[j + 6] : 0,
                              j + 7 < cnt ? d[j + 7] : 0);
                    break;
                }
            }
        }
        flushBos();
        uint64_t seq = 0;
        phxn_cs_ib ib0;
        bcopy(in + sizeof(h), &ib0, sizeof(ib0));
        if (!ensureVmid())
            return kIOReturnNoResources;
        const IOReturn r = owner->gpuSubmit(vmid, ib0.ip_type == PHX_AMDGPU_HW_IP_GFX, va, dw, h.num_ibs, &seq);
        if (r != kIOReturnSuccess)
            return r;
        lastSeq = seq;
        emitted = seq;   // externalMethod already holds `lock`
        if (fb) {
            pendFenceHandle = h.fence_handle;
            pendFenceOffset = h.fence_offset;
            pendFenceSeq = seq;
        }
        a->scalarOutput[0] = seq;
        return kIOReturnSuccess;
    }
    // E1: no GPU behind the call; the submission retires at once, as in the mock.
    const uint64_t seq = ++emitted;
    if (fb)
        bcopy(&seq, fb->kva + h.fence_offset, sizeof(seq));
    a->scalarOutput[0] = seq;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doDbgSmu(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 2, 0, 0) || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    switch (a->scalarInput[0]) {
    case PHX_PPSMC_TestMessage:
    case PHX_PPSMC_GetPmfwVersion:
    case PHX_PPSMC_GetDriverIfVersion:
    case PHX_PPSMC_GetEnabledSmuFeatures:
    case PHX_PPSMC_AllowGfxOff:
    case PHX_PPSMC_DisallowGfxOff:
    case PHX_PPSMC_EnableGfxImu:
    case PHX_PPSMC_GetGfxclkFrequency:
    case PHX_PPSMC_GetFclkFrequency:
    case PHX_PPSMC_SetHardMinGfxClk:
    case PHX_PPSMC_SetSoftMaxGfxClk:
    case PHX_PPSMC_SetHardMinFclkByFreq:
    case PHX_PPSMC_SetSoftMaxFclkByFreq:
    case PHX_PPSMC_SetHardMinSocclkByFreq:
    case PHX_PPSMC_SetSoftMaxSocclkByFreq:
        break;
    default:
        return kIOReturnNotPermitted;
    }
    uint32_t resp = 0, after = 0;
    IOReturn r = owner->smuMessage(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]),
                                   &resp, &after);
    a->scalarOutput[0] = resp;
    a->scalarOutput[1] = after;
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspRing(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 4, 0, 0))
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspRing(static_cast<uint32_t>(a->scalarInput[0]), a->scalarInput[1], out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgVramRead(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (a->scalarInputCount != 2 || a->scalarOutputCount != 0 || a->structureInputSize != 0)
        return kIOReturnBadArgument;
    uint64_t off = a->scalarInput[0], count = a->scalarInput[1];
    if (count < 1 || count > 256 || off > 0xFFFFFFFFull || a->structureOutputSize != count * 4)
        return kIOReturnBadArgument;
    return owner->vramRead(static_cast<uint32_t>(off), static_cast<uint32_t>(count),
                           static_cast<uint32_t *>(a->structureOutput));
}

IOReturn IOAccelPhxNativeClient::doDbgPspSubmit(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 1, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspSubmit(static_cast<uint32_t>(a->scalarInput[0]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgFwStage(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (a->scalarInputCount != 1 || a->scalarOutputCount != 0 || a->structureOutputSize != 0)
        return kIOReturnBadArgument;
    if (a->structureInputDescriptor || !a->structureInput || a->structureInputSize == 0 ||
        a->structureInputSize > 4096 || a->scalarInput[0] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    return owner->fwStage(static_cast<uint32_t>(a->scalarInput[0]), a->structureInput, a->structureInputSize);
}

IOReturn IOAccelPhxNativeClient::doDbgLoadIpFw(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull ||
        a->scalarInput[2] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->loadIpFw(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]),
                                 static_cast<uint32_t>(a->scalarInput[2]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspCtl(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 4, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspCtl(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]), out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgPspRaw(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 7, 4, 0, 0))
        return kIOReturnBadArgument;
    uint32_t w[6];
    for (int i = 0; i < 6; i++)
        w[i] = static_cast<uint32_t>(a->scalarInput[i + 1]);
    uint64_t out[4] = {0, 0, 0, 0};
    IOReturn r = owner->pspRaw(static_cast<uint32_t>(a->scalarInput[0]), w, out);
    for (int i = 0; i < 4; i++)
        a->scalarOutput[i] = out[i];
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgWriteReg(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 1, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    uint32_t rb = 0;
    IOReturn r = owner->writeReg(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]), &rb);
    a->scalarOutput[0] = rb;
    return r;
}

IOReturn IOAccelPhxNativeClient::doDbgVramWrite(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 2, 0, 0, 0) || a->scalarInput[0] > 0xFFFFFFFFull || a->scalarInput[1] > 0xFFFFFFFFull)
        return kIOReturnBadArgument;
    return owner->vramWrite(static_cast<uint32_t>(a->scalarInput[0]), static_cast<uint32_t>(a->scalarInput[1]));
}

IOReturn IOAccelPhxNativeClient::doDbgDoorbell(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 0, 0, 0))
        return kIOReturnBadArgument;
    return owner->doorbellWrite(a->scalarInput[0], a->scalarInput[1] | (a->scalarInput[2] << 32));
}

IOReturn IOAccelPhxNativeClient::doDbgSysMem(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 3, 2, 0, 0))
        return kIOReturnBadArgument;
    return owner->sysMem(a->scalarInput[0], a->scalarInput[1], a->scalarInput[2], &a->scalarOutput[0], &a->scalarOutput[1]);
}

IOReturn IOAccelPhxNativeClient::doDbgAdoptQueue(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 6, 0, 0, 0))
        return kIOReturnBadArgument;
    return owner->adoptQueue(a->scalarInput);
}

IOReturn IOAccelPhxNativeClient::doDbgClaim(IOExternalMethodArguments *a) {
    if (!owner->debugEnabled())
        return kIOReturnUnsupported;
    if (!privileged)
        return kIOReturnNotPrivileged;
    if (!shape(a, 1, 0, 0, 0) || a->scalarInput[0] != 1)
        return kIOReturnBadArgument;
    return owner->claimAccel();
}

// Write back and invalidate the CPU caches over every BO: CPU-written data must be in memory before the GPU reads
// it, and the CPU must not read stale lines after the GPU wrote. Coarse: WindowServer's BOs (full-screen surfaces
// among them) made this its hottest loop, hence debug.phxbos_flush.
void IOAccelPhxNativeClient::flushBos() {
    const int mode = gBosFlush;
    if (!mode)
        return;
    uint64_t bytes = 0;
    for (uint32_t h = 1; h < PHXN_MAX_BOS; h++) {
        if (!bos[h].kva)   // host imports: the CPU's own pages, which the GPU reads and writes snooped
            continue;
        const uint8_t *p = bos[h].kva;
        if (mode == 1) {
            for (uint64_t o = 0; o < bos[h].size; o += 64)
                __asm__ __volatile__("clflushopt (%0)" ::"r"(p + o) : "memory");
        } else {
            for (uint64_t o = 0; o < bos[h].size; o += 64)
                __asm__ __volatile__("clflush (%0)" ::"r"(p + o) : "memory");
        }
        bytes += bos[h].size;
    }
    __asm__ __volatile__("mfence" ::: "memory");
    OSAddAtomic64((SInt64)bytes, &gBosFlushBytes);
}

// WindowServer's composite marks for PhxAccel: the swap of a surface marked pending waits in NMXAccel until it is marked
// ready (nvSurfWaitReady), so WindowServer need not wait for each composite itself (commitAndWaitUntilSubmitted held
// its main thread ~1.7 ms a frame, 2026-10-26). They reached NMXAccel through NVIDIA's RM escape before; here the
// accelerator's own entry (callPlatformFunction "nvSurfVram", NVRM_ESC_SURF_DIRTY with the GPU-mark plane bit) takes
// them. The accelerator checks the caller is WindowServer.
struct PhxSurfShareEsc {   // NVRMSurfShareEsc, src/phxaccel/nvrm_surfshare_abi.h (version 2)
    uint32_t version, surfaceID, plane, hClient, hSrcClient, hSrcMemory, status, pad;
    uint64_t size;
};
static_assert(sizeof(PhxSurfShareEsc) == 40, "NVRMSurfShareEsc layout");
IOReturn IOAccelPhxNativeClient::doSurfGpuMark(IOExternalMethodArguments *a) {
    if (!shape(a, 2, 1, 0, 0))
        return kIOReturnBadArgument;
    static IOService *accel;
    static const OSSymbol *fn;
    if (!accel) {
        IOService *s = IOService::copyMatchingService(IOService::serviceMatching("NMXAccel"));
        if (s && !OSCompareAndSwapPtr(nullptr, s, (void *volatile *)&accel))
            s->release();
        if (!accel)
            return kIOReturnNotFound;
    }
    if (!fn) {
        const OSSymbol *f = OSSymbol::withCStringNoCopy("nvSurfVram");
        if (f && !OSCompareAndSwapPtr(nullptr, (void *)f, (void *volatile *)&fn))
            f->release();
        if (!fn)
            return kIOReturnNoMemory;
    }
    PhxSurfShareEsc e{};
    e.version = 2;
    e.surfaceID = static_cast<uint32_t>(a->scalarInput[0]);
    e.plane = 0x80000000u | (a->scalarInput[1] ? 1u : 0u);   // NVRM_SS_PLANE_GPU_MARK | pending
    e.status = 0x80000002u;                                    // NVRM_SS_OFF unless the accelerator answers
    const IOReturn r = accel->callPlatformFunction(fn, false, &e, task, reinterpret_cast<void *>(0xE3), nullptr);   // NVRM_ESC_SURF_DIRTY
    a->scalarOutput[0] = r == kIOReturnSuccess ? e.status : 0x80000002u;
    return kIOReturnSuccess;
}

IOReturn IOAccelPhxNativeClient::doWaitSeq(IOExternalMethodArguments *a) {
    if (!hello)
        return kIOReturnNotReady;
    if (!shape(a, 3, 3, 0, 0))
        return kIOReturnBadArgument;
    IOLockLock(lock);
    uint64_t target = a->scalarInput[0];
    const uint64_t em = emitted;
    IOLockUnlock(lock);
    if (target == PHXN_SEQ_LAST)
        target = em;
    if (target > em)
        return kIOReturnBadArgument;
    if (owner->gpuReady()) {
        // E8: poll the RELEASE_MEM fence the adopted queue writes after each submission
        uint64_t timeout = a->scalarInput[1];
        if (timeout > PHXN_WAIT_CAP_NS)
            timeout = PHXN_WAIT_CAP_NS;
        uint64_t retired = owner->gpuRetired();
        for (uint64_t waited = 0; retired < target && waited < timeout; ) {
            if (waited < 1000000ull) {
                // Short wait (< 1 ms): fast poll with 50 us delay for low latency
                IODelay(50);
                waited += 50000ull;
            } else {
                // Extended wait (>= 1 ms): yield CPU to scheduler to avoid starving WindowServer & system threads
                IOSleep(1);
                waited += 1000000ull;
            }
            retired = owner->gpuRetired();
        }
        if (retired >= target) {
            // drop CPU cache lines so the CPU sees what the GPU wrote; under the lock, as another thread of the same
            // client (WindowServer) freed a BO mid-flush and the walk dereferenced its NULL descriptor (panic 2026-10-20)
            IOLockLock(lock);
            flushBos();
            if (pendFenceSeq && pendFenceSeq <= retired) {
                Bo *fb = findBo(pendFenceHandle);
                if (fb && fb->kva && pendFenceOffset + 8 <= fb->size)
                    bcopy(&pendFenceSeq, fb->kva + pendFenceOffset, 8);
                pendFenceSeq = 0;
            }
            IOLockUnlock(lock);
        }
        a->scalarOutput[0] = retired < target;
        a->scalarOutput[1] = retired;
        a->scalarOutput[2] = em;
        return kIOReturnSuccess;
    }
    a->scalarOutput[0] = 0;   // not busy: E1 retires every submission at once
    a->scalarOutput[1] = em;
    a->scalarOutput[2] = em;
    return kIOReturnSuccess;
}
