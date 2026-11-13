// PhxCPPC.kext: AMD CPPC (Collaborative Processor Performance Control) for Zen 4 APUs on macOS.
//
// macOS has no AMD CPU performance driver: the cores run the firmware's legacy P-state with core performance boost,
// so even light load (software video decode) boosts to the top frequency. Linux amd-pstate (active mode) instead enables
// CPPC and leaves the frequency to the hardware, guided by min/max performance and an energy-performance preference
// (EPP, 0 = performance .. 255 = power). This kext exposes the same knobs, per sysctl, and changes nothing until told to.
//
//   debug.phx_cppc_supported   CPUID Fn8000_0008 EBX[27] (MSR CPPC)
//   debug.phx_cppc_dump        per CPU: CPPC enable, request (max/min/des/epp), caps, boost, P-state, APERF/MPERF/TSC
//                              deltas over 200 ms (effective clock = tsc_mhz * aperf / tsc while busy)
//   debug.phx_cppc_enable      write 1: MSR_AMD_CPPC_ENABLE on every CPU (one way until reset, as amd-pstate does)
//   debug.phx_cppc_epp/_min/_max  request fields (-1 = keep); debug.phx_cppc_apply write 1: write them on every CPU
//   debug.phx_cppc_boost       read: core performance boost on (HWCR CpbDis clear); write 0/1 to change it
//
// MSRs: AMD64 APM vol. 2 / Linux arch/x86/include/asm/msr-index.h.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <kern/thread_call.h>

extern "C" {
void mp_rendezvous_no_intrs(void (*action)(void *), void *arg);
int cpu_number(void);
}

static constexpr uint32_t kMsrCppcCap1 = 0xC00102B0, kMsrCppcEnable = 0xC00102B1, kMsrCppcReq = 0xC00102B3;
static constexpr uint32_t kMsrHwcr = 0xC0010015, kMsrPStateStat = 0xC0010063;
static constexpr uint32_t kMsrMperfRo = 0xC00000E7, kMsrAperfRo = 0xC00000E8, kMsrMperf = 0xE7, kMsrAperf = 0xE8;
static uint32_t gMsrMperf = kMsrMperf, gMsrAperf = kMsrAperf;   // read-only copies when CPUID says EffFreqRO
static constexpr uint64_t kHwcrCpbDis = 1ull << 25;
static constexpr int kMaxCpus = 64;

static inline uint64_t rdmsr(uint32_t m) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
static inline void wrmsr(uint32_t m, uint64_t v) {
    __asm__ __volatile__("wrmsr" ::"c"(m), "a"(static_cast<uint32_t>(v)), "d"(static_cast<uint32_t>(v >> 32)));
}
static inline uint64_t rdtsc64() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}
static inline void cpuid(uint32_t leaf, uint32_t r[4]) {
    __asm__ __volatile__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

struct CpuSnap {
    uint64_t cap1, en, req, hwcr, pstat, aperf, mperf, tsc;
    int seen;
};
static CpuSnap gSnap[kMaxCpus], gSnap2[kMaxCpus];
static volatile SInt32 gNcpu = 0;
static int gSupported = 0, gEpp = -1, gMin = -1, gMax = -1, gApplied = 0, gEnabled = 0;
static IOLock *gLock = nullptr;

static void snapOne(void *arg) {
    CpuSnap *s = static_cast<CpuSnap *>(arg);
    const int c = cpu_number();
    if (c < 0 || c >= kMaxCpus)
        return;
    CpuSnap &x = s[c];
    x.tsc = rdtsc64();
    x.aperf = rdmsr(gMsrAperf);
    x.mperf = rdmsr(gMsrMperf);
    x.cap1 = rdmsr(kMsrCppcCap1);
    x.en = rdmsr(kMsrCppcEnable);
    x.req = rdmsr(kMsrCppcReq);
    x.hwcr = rdmsr(kMsrHwcr);
    x.pstat = rdmsr(kMsrPStateStat);
    x.seen = 1;
}

struct WriteOp {
    int what;          // 1 enable CPPC, 2 request, 3 boost
    uint64_t value;    // request fields (masked by keep) or boost on/off
    uint64_t keepMask; // request bits to keep from the current value
    bool ownMax, ownMin; // request: max = this CPU's highest perf, min = its lowest nonlinear perf (CAP1)
};
static void writeOne(void *arg) {
    const WriteOp *w = static_cast<const WriteOp *>(arg);
    if (w->what == 1) {
        if (!(rdmsr(kMsrCppcEnable) & 1))
            wrmsr(kMsrCppcEnable, 1);
    } else if (w->what == 2) {
        const uint64_t cur = rdmsr(kMsrCppcReq);
        uint64_t v = w->value;
        if (w->ownMax || w->ownMin) {
            // the cores differ (preferred cores: highest 196..232 on a Ryzen 7 255), so one value for all would cap
            // the best ones; amd-pstate also writes each CPU's own limits
            const uint64_t c1 = rdmsr(kMsrCppcCap1);
            if (w->ownMax)
                v = (v & ~0xffull) | ((c1 >> 24) & 0xff);
            if (w->ownMin)
                v = (v & ~0xff00ull) | (((c1 >> 8) & 0xff) << 8);
        }
        wrmsr(kMsrCppcReq, (cur & w->keepMask) | (v & ~w->keepMask));
    } else if (w->what == 3) {
        const uint64_t h = rdmsr(kMsrHwcr);
        wrmsr(kMsrHwcr, w->value ? (h & ~kHwcrCpbDis) : (h | kHwcrCpbDis));
    }
}

static void snapshot(CpuSnap *s) {
    bzero(s, sizeof(CpuSnap) * kMaxCpus);
    mp_rendezvous_no_intrs(snapOne, s);
    int n = 0;
    for (int i = 0; i < kMaxCpus; i++)
        n += s[i].seen;
    gNcpu = n;
}

// ---- sysctls -------------------------------------------------------------------------------------------------------
static int handleDump(struct sysctl_oid *, void *, int, struct sysctl_req *req) {
    if (!gSupported)
        return SYSCTL_OUT(req, "CPPC not supported\n", 19);
    IOLockLock(gLock);
    snapshot(gSnap);
    IOSleep(200);
    snapshot(gSnap2);
    const size_t cap = 160 * (kMaxCpus + 3);
    char *buf = static_cast<char *>(IOMalloc(cap));
    size_t o = 0;
    o += snprintf(buf + o, cap - o, "cpu en  max min des epp | lowest lnl nominal highest | boost pst | aperf_d mperf_d tsc_d\n");
    for (int i = 0; i < kMaxCpus && o < cap - 160; i++) {
        const CpuSnap &a = gSnap[i], &b = gSnap2[i];
        if (!a.seen || !b.seen)
            continue;
        const uint64_t r = b.req, c1 = b.cap1;
        o += snprintf(buf + o, cap - o, "%3d %2llu %4llu %3llu %3llu %3llu | %6llu %3llu %7llu %7llu | %5s %3llu | %llu %llu %llu\n", i,
                      b.en & 1, r & 0xff, (r >> 8) & 0xff, (r >> 16) & 0xff, (r >> 24) & 0xff, c1 & 0xff,
                      (c1 >> 8) & 0xff, (c1 >> 16) & 0xff, (c1 >> 24) & 0xff, (b.hwcr & kHwcrCpbDis) ? "off" : "on",
                      b.pstat & 7, b.aperf - a.aperf, b.mperf - a.mperf, b.tsc - a.tsc);
    }
    IOLockUnlock(gLock);
    const int err = SYSCTL_OUT(req, buf, o);
    IOFree(buf, cap);
    return err;
}

static int handleEnable(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
    int v = gEnabled;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (v != 1 || !gSupported)
        return EINVAL;
    IOLockLock(gLock);
    WriteOp w = {1, 0, 0};
    mp_rendezvous_no_intrs(writeOne, &w);
    snapshot(gSnap);
    gEnabled = (gSnap[0].en & 1) ? 1 : 0;
    IOLockUnlock(gLock);
    IOLog("PhxCPPC: CPPC enable -> %d\n", gEnabled);
    return 0;
}

static int handleApply(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
    int v = gApplied;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (v != 1 || !gSupported)
        return EINVAL;
    IOLockLock(gLock);
    snapshot(gSnap);
    const uint64_t c1 = gSnap[0].cap1;
    const int lowest = static_cast<int>(c1 & 0xff), highest = static_cast<int>((c1 >> 24) & 0xff);
    uint64_t val = 0, keep = ~0ull;
    bool ownMax = false, ownMin = false;
    if (gMax == -2) {
        ownMax = true;
        keep &= ~0xffull;
    } else if (gMax >= 0) {
        if (gMax < lowest || gMax > highest) { IOLockUnlock(gLock); return EINVAL; }
        val |= static_cast<uint64_t>(gMax);
        keep &= ~0xffull;
    }
    if (gMin == -2) {
        ownMin = true;
        keep &= ~0xff00ull;
    } else if (gMin >= 0) {
        if (gMin < lowest || gMin > highest || (gMax >= 0 && gMin > gMax)) { IOLockUnlock(gLock); return EINVAL; }
        val |= static_cast<uint64_t>(gMin) << 8;
        keep &= ~0xff00ull;
    }
    if (gEpp >= 0) {
        if (gEpp > 255) { IOLockUnlock(gLock); return EINVAL; }
        val |= static_cast<uint64_t>(gEpp) << 24;
        keep &= ~0xff000000ull;
    }
    // desired performance 0: autonomous selection (amd-pstate active / EPP mode)
    val |= 0;
    keep &= ~0xff0000ull;
    WriteOp w = {2, val, keep, ownMax, ownMin};
    mp_rendezvous_no_intrs(writeOne, &w);
    gApplied = 1;
    IOLockUnlock(gLock);
    IOLog("PhxCPPC: request max %d min %d epp %d (desired 0) applied (-2: each CPU's own)\n", gMax, gMin, gEpp);
    return 0;
}

static int handleBoost(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
    IOLockLock(gLock);
    snapshot(gSnap);
    int v = (gSnap[0].hwcr & kHwcrCpbDis) ? 0 : 1;
    IOLockUnlock(gLock);
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr)
        return err;
    if (v != 0 && v != 1)
        return EINVAL;
    IOLockLock(gLock);
    WriteOp w = {3, static_cast<uint64_t>(v), 0};
    mp_rendezvous_no_intrs(writeOne, &w);
    IOLockUnlock(gLock);
    IOLog("PhxCPPC: core performance boost -> %d\n", v);
    return 0;
}

SYSCTL_INT(_debug, OID_AUTO, phx_cppc_supported, CTLFLAG_RD | CTLFLAG_LOCKED, &gSupported, 0, "MSR CPPC (CPUID Fn8000_0008 EBX[27])");
SYSCTL_PROC(_debug, OID_AUTO, phx_cppc_dump, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, handleDump, "A",
            "per-CPU CPPC state");
SYSCTL_PROC(_debug, OID_AUTO, phx_cppc_enable, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, handleEnable, "I",
            "write 1: enable CPPC on every CPU (until reset)");
SYSCTL_INT(_debug, OID_AUTO, phx_cppc_epp, CTLFLAG_RW | CTLFLAG_LOCKED, &gEpp, 0, "EPP 0..255 (-1 keep)");
SYSCTL_INT(_debug, OID_AUTO, phx_cppc_min, CTLFLAG_RW | CTLFLAG_LOCKED, &gMin, 0, "min perf (-1 keep, -2 each CPU's lowest nonlinear)");
SYSCTL_INT(_debug, OID_AUTO, phx_cppc_max, CTLFLAG_RW | CTLFLAG_LOCKED, &gMax, 0, "max perf (-1 keep, -2 each CPU's highest)");
SYSCTL_PROC(_debug, OID_AUTO, phx_cppc_apply, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, handleApply, "I",
            "write 1: write epp/min/max (desired 0) on every CPU");
SYSCTL_PROC(_debug, OID_AUTO, phx_cppc_boost, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, handleBoost, "I",
            "core performance boost (HWCR CpbDis clear) 1 on / 0 off");
static struct sysctl_oid *const kOids[] = {
    &sysctl__debug_phx_cppc_supported, &sysctl__debug_phx_cppc_dump, &sysctl__debug_phx_cppc_enable,
    &sysctl__debug_phx_cppc_epp,       &sysctl__debug_phx_cppc_min,  &sysctl__debug_phx_cppc_max,
    &sysctl__debug_phx_cppc_apply,     &sysctl__debug_phx_cppc_boost,
};

// ---- the service -----------------------------------------------------------------------------------------------------
class PhxCPPC : public IOService {
    OSDeclareDefaultStructors(PhxCPPC)
    thread_call_t regCall{nullptr};
    bool registered{false};
    static void late(thread_call_param_t self, thread_call_param_t);

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};
OSDefineMetaClassAndStructors(PhxCPPC, IOService)

// The kext is injected by the bootloader and starts long before BSD: sysctls go up 20 s later.
void PhxCPPC::late(thread_call_param_t self, thread_call_param_t) {
    PhxCPPC *me = static_cast<PhxCPPC *>(self);
    for (struct sysctl_oid *o : kOids)
        sysctl_register_oid(o);
    me->registered = true;
    IOLockLock(gLock);
    snapshot(gSnap);
    IOLockUnlock(gLock);
    const uint64_t c1 = gSnap[0].cap1, r = gSnap[0].req;
    gEnabled = (gSnap[0].en & 1) ? 1 : 0;
    IOLog("PhxCPPC: %d CPUs, CPPC enabled %d, caps lowest %llu nonlinear %llu nominal %llu highest %llu, request max %llu "
          "min %llu des %llu epp %llu, boost %s\n",
          static_cast<int>(gNcpu), gEnabled, c1 & 0xff, (c1 >> 8) & 0xff, (c1 >> 16) & 0xff, (c1 >> 24) & 0xff,
          r & 0xff, (r >> 8) & 0xff, (r >> 16) & 0xff, (r >> 24) & 0xff, (gSnap[0].hwcr & kHwcrCpbDis) ? "off" : "on");
}

bool PhxCPPC::start(IOService *provider) {
    if (!IOService::start(provider))
        return false;
    uint32_t r[4];
    cpuid(0, r);
    const bool amd = r[1] == 0x68747541 && r[3] == 0x69746e65 && r[2] == 0x444d4163;   // "AuthenticAMD"
    cpuid(0x80000000, r);
    const uint32_t maxExt = r[0];
    if (amd && maxExt >= 0x80000008) {
        cpuid(0x80000008, r);
        gSupported = (r[1] >> 27) & 1;
        cpuid(0x80000007, r);
        if ((r[3] >> 10) & 1) {          // EffFreqRO: MPerfReadOnly / APerfReadOnly
            gMsrMperf = kMsrMperfRo;
            gMsrAperf = kMsrAperfRo;
        }
    }
    if (!gSupported) {
        IOLog("PhxCPPC: no MSR CPPC on this CPU, not starting\n");
        return false;
    }
    gLock = IOLockAlloc();
    regCall = thread_call_allocate(late, this);
    if (!gLock || !regCall)
        return false;
    uint64_t d;
    clock_interval_to_deadline(20000, kMillisecondScale, &d);
    thread_call_enter_delayed(regCall, d);
    registerService();
    return true;
}

void PhxCPPC::stop(IOService *provider) {
    if (regCall) {
        thread_call_cancel_wait(regCall);
        thread_call_free(regCall);
        regCall = nullptr;
    }
    if (registered)
        for (struct sysctl_oid *o : kOids)
            sysctl_unregister_oid(o);
    IOService::stop(provider);
}
