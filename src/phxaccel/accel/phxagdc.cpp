// PhxAGDC: the AppleGraphicsDeviceControl service WindowServer asks, when it starts, for the GPU's framebuffers (vendor
// command 0x980). CoreDisplay builds one display pipe per framebuffer in the reply; with no such service it logs "Unable to
// find display pipe for IOFB service", composites the panel in software, and the software renderer aborts on layers the
// Metal compositor made (2026-10-20). Modeled on NullMoth's NVRMAGDC: one slot, PhxFB, created only on
// `sysctl debug.phxagdc=1` (the M2 arming step), never at boot.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <sys/sysctl.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include "AppleGraphicsDeviceControl_re.h"
#include "nvrm-agdc-k5.h"

#define AGLOG(fmt, ...) IOLog("PhxAGDC: " fmt "\n", ##__VA_ARGS__)

#pragma pack(push, 1)
struct PhxAGDCVendorInfo {
    uint32_t version;
    char     vendor[0x20];
    uint32_t vendorID;
    uint32_t vendorClass;
};
struct PhxAGDCGPUCapability {
    uint64_t portMask;
    uint64_t portFeatureA, portFeatureB, portFeatureC;
    uint32_t countA, countB, countC, countD;
    uint32_t maxFramebuffers;
    IOService *pciDevice;
    IOService *framebuffer[20];
};
#pragma pack(pop)
static_assert(sizeof(PhxAGDCVendorInfo) == 0x2c, "AGDCVendorInfo_t is 0x2c");
static_assert(sizeof(PhxAGDCGPUCapability) == 0xdc, "AGDCGPUCapability_t is 0xdc");

static IOService *gPci = nullptr, *gFB = nullptr;   // held while the service is up
extern IOService *gPhxHDMI;
static SInt64 gCmds = 0, gRefused = 0, gFbReplies = 0, gK5 = 0;
static int gLastRefused = 0, gLogged = 0;

// The header sizes the base as 0x110 (15.7.9); the padding keeps a larger Tahoe base inside this object, and
// create() refuses when the running base says it is larger still.
class PhxDeviceControl : public AppleGraphicsDeviceControl {
    OSDeclareDefaultStructors(PhxDeviceControl)
    uint8_t _pad[0x300];
public:
    using AppleGraphicsDeviceControl::vendor_doDeviceAttribute;
    IOReturn vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize, unsigned long *out,
                                      unsigned long *outSize, IOExternalMethodArguments *args) override;
};
OSDefineMetaClassAndStructors(PhxDeviceControl, AppleGraphicsDeviceControl)

// CVT reduced-blanking timing for the panel's mode (PhxFB's IOFBConfig), in nvrm-agdc-k5.h's word layout.
static void panelTiming(uint32_t t[NVK5_TIMING_WORDS])
{
    uint32_t w = 2560, h = 1600, mhz = 60000, ht = 0, vt = 0;
    if (gFB) {
        const char *keys[5] = { "PhxFBWidth", "PhxFBHeight", "PhxFBRefreshMilliHz", "PhxFBHTotal", "PhxFBVTotal" };
        uint32_t *dst[5] = { &w, &h, &mhz, &ht, &vt };
        for (int k = 0; k < 5; k++) {
            OSObject *o = gFB->copyProperty(keys[k]);
            if (OSNumber *n = OSDynamicCast(OSNumber, o)) *dst[k] = n->unsigned32BitValue();
            OSSafeReleaseNULL(o);
        }
    }
    // the totals and refresh PhxAMDFB read off the display engine; CVT-RB (48/32/80 horizontal, 3/6/37 vertical) otherwise
    const uint32_t htot = ht > w ? ht : w + 160, vtot = vt > h ? vt : h + 46;
    bzero(t, NVK5_TIMING_WORDS * sizeof(uint32_t));
    t[NVK5_T_TAG] = NVK5_TIMING_TAG;
    t[NVK5_T_HVIS] = w; t[NVK5_T_HSS] = w + 48; t[NVK5_T_HSE] = w + 80; t[NVK5_T_HTOT] = htot;
    t[NVK5_T_VVIS] = h; t[NVK5_T_VSS] = h + 3; t[NVK5_T_VSE] = h + 9; t[NVK5_T_VTOT] = vtot;
    t[NVK5_T_PCLK] = (uint32_t)((uint64_t)htot * vtot * mhz / 1000);
    t[NVK5_T_REFRESH_MHZ] = mhz; t[NVK5_T_HSYNCPOS] = 1; t[NVK5_T_VSYNCPOS] = 0;
}

IOReturn PhxDeviceControl::vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                                    unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *)
{
    OSAddAtomic64(1, &gCmds);
    if (cmd == 1) {
        if (!out || !outSize || *outSize != sizeof(PhxAGDCVendorInfo)) return kIOReturnBadArgument;
        PhxAGDCVendorInfo *v = (PhxAGDCVendorInfo *)out;
        bzero(v, sizeof *v);
        v->version = 0x30000; strlcpy(v->vendor, "AMD", sizeof v->vendor); v->vendorID = 0x1002; v->vendorClass = 2;
        return kIOReturnSuccess;
    }
    if (cmd == 0x980) {
        if (!out || !outSize || *outSize != sizeof(PhxAGDCGPUCapability)) return kIOReturnBadArgument;
        PhxAGDCGPUCapability *c = (PhxAGDCGPUCapability *)out;
        bzero(c, sizeof *c);
        c->portMask = (1ull << 1) | (1ull << 2);
        c->countA = c->countB = c->countC = c->countD = 2;
        c->maxFramebuffers = gPhxHDMI ? 2 : 1; c->pciDevice = gPci;
        c->framebuffer[0] = gFB;
        c->framebuffer[1] = gPhxHDMI ? gPhxHDMI : gFB;
        if (OSAddAtomic64(1, &gFbReplies) < 4) AGLOG("0x980: %u framebuffers (PhxFB id %#llx, HDMI id %#llx), 2 ports", c->maxFramebuffers, gFB ? gFB->getRegistryEntryID() : 0ull, gPhxHDMI ? gPhxHDMI->getRegistryEntryID() : 0ull);
        return kIOReturnSuccess;
    }
    // IOPresentment asks for the panel's link (0x921) and plane capabilities (0x711) before it presents through the
    // display pipe; refused, it logged "Unable to acquire capabilities ... 0x80e" and WindowServer composited the panel
    // in software. The firmware lit the panel without telling its timing, so the answer carries the CVT reduced-blanking
    // timing of the mode (NullMoth's K5 replies, nvrm-agdc-k5.h).
    if (cmd == NVK5_CMD_LINKCONFIG || cmd == NVK5_CMD_FBCAPEX) {
        uint32_t t0[NVK5_TIMING_WORDS], t1[NVK5_TIMING_WORDS];
        panelTiming(t0);
        // t1 for external 1080p60
        bzero(t1, sizeof(t1));
        t1[NVK5_T_TAG] = NVK5_TIMING_TAG;
        t1[NVK5_T_HVIS] = 1920; t1[NVK5_T_HSS] = 1920 + 88; t1[NVK5_T_HSE] = 1920 + 88 + 44; t1[NVK5_T_HTOT] = 2200;
        t1[NVK5_T_VVIS] = 1080; t1[NVK5_T_VSS] = 1080 + 4;  t1[NVK5_T_VSE] = 1080 + 4 + 5;   t1[NVK5_T_VTOT] = 1125;
        t1[NVK5_T_PCLK] = 148500000; // 148.5 MHz standard 1080p60
        t1[NVK5_T_REFRESH_MHZ] = 60000;
        t1[NVK5_T_HSYNCPOS] = 1; t1[NVK5_T_VSYNCPOS] = 1;

        struct nvk5_head heads[2] = {
            { t0, sizeof(t0) },
            { t1, sizeof(t1) }
        };
        int why = 0; int32_t idxOut = -1;
        const int r = nvk5_answer_heads(cmd, in, inSize, out, outSize ? *outSize : 0, heads, 2, &why, &idxOut);
        if (r == NVK5_R_LINK || r == NVK5_R_CAPS) {
            if (OSAddAtomic64(1, &gK5) < 8) AGLOG("command %#x answered (%s head %d)", cmd, r == NVK5_R_LINK ? "link config" : "plane scaler", idxOut);
            return kIOReturnSuccess;
        }
        if (gLogged < 40) { gLogged++; AGLOG("command %#x not answered (result %d, why %#x, head %d)", cmd, r, why, idxOut); }
        if (r == NVK5_R_BADARG) return kIOReturnBadArgument;
        if (r == NVK5_R_NOTFOUND) return kIOReturnNotFound;
    }
    OSAddAtomic64(1, &gRefused); gLastRefused = (int)cmd;
    if (gLogged < 40) { gLogged++; AGLOG("command %#x (in %lu bytes) -> Unsupported [%d/40]", cmd, inSize, gLogged); }
    return kIOReturnUnsupported;
}

static PhxDeviceControl *gAgdc = nullptr;
static int gState = 0, gWant = 0;
static thread_call_t gCall = nullptr;
static SInt32 gBusy = 0;

static void dropTargets() { OSSafeReleaseNULL(gFB); gPci = nullptr; }

static void agdcWork(thread_call_param_t, thread_call_param_t)
{
    OSIncrementAtomic(&gBusy);
    if (gWant == 1 && !gAgdc) do {
        const OSSymbol *name = OSSymbol::withCStringNoCopy("AppleGraphicsDeviceControl");
        const OSMetaClass *base = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
        OSSafeReleaseNULL(name);
        const unsigned baseSize = base ? base->getClassSize() : 0;
        AGLOG("running AppleGraphicsDeviceControl is %#x bytes (header %#lx, room %#lx)", baseSize,
              sizeof(AppleGraphicsDeviceControl), sizeof(PhxDeviceControl));
        if (!baseSize || baseSize > sizeof(PhxDeviceControl)) { gState = -6; AGLOG("base class larger than this object: not creating"); break; }
        OSDictionary *m = IOService::serviceMatching("PhxAMDFB");
        IOService *fb = m ? IOService::copyMatchingService(m) : nullptr;
        OSSafeReleaseNULL(m);
        IOService *p = fb ? fb->getProvider() : nullptr;
        if (!fb || !OSDynamicCast(IOPCIDevice, p)) { gState = -1; AGLOG("no PhxFB on a PCI device yet"); OSSafeReleaseNULL(fb); break; }
        gFB = fb; gPci = p;
        PhxDeviceControl *d = OSTypeAlloc(PhxDeviceControl);
        if (!d || !d->init(nullptr)) { gState = -3; AGLOG("alloc/init failed"); OSSafeReleaseNULL(d); dropTargets(); break; }
        if (!d->attach(p)) { gState = -4; AGLOG("attach(%s) failed", p->getName()); d->release(); dropTargets(); break; }
        if (!d->start(p)) { gState = -5; AGLOG("AppleGraphicsDeviceControl::start failed"); d->detach(p); d->release(); dropTargets(); break; }
        gAgdc = d; gState = 1;
        AGLOG("UP on %s: one framebuffer slot (PhxFB); vendor calls so far %lld", p->getName(), gCmds);
    } while (0);
    else if (gWant == 0 && gAgdc) {
        PhxDeviceControl *d = gAgdc; gAgdc = nullptr;
        bool ok = d->terminate(kIOServiceRequired | kIOServiceSynchronous);
        d->release(); dropTargets(); gState = 2;
        AGLOG("terminate -> %d", ok);
    }
    OSDecrementAtomic(&gBusy);
}

static int phxagdc_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gState;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 0 && v != 1) return EINVAL;
    gWant = v;
    if (!gCall) gCall = thread_call_allocate(agdcWork, nullptr);
    if (!gCall) return ENOMEM;
    thread_call_enter(gCall);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxagdc, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, phxagdc_sysctl, "I",
            "write 1 = create the AppleGraphicsDeviceControl that reports PhxFB to WindowServer, 0 = terminate it; reads the state (1 up, <0 failed step)");
SYSCTL_QUAD(_debug, OID_AUTO, phxagdc_cmds, CTLFLAG_RD | CTLFLAG_LOCKED, &gCmds, "AGDC vendor calls received");
SYSCTL_QUAD(_debug, OID_AUTO, phxagdc_fb_replies, CTLFLAG_RD | CTLFLAG_LOCKED, &gFbReplies, "0x980 framebuffer-list replies");
SYSCTL_QUAD(_debug, OID_AUTO, phxagdc_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gRefused, "AGDC vendor calls answered Unsupported");
SYSCTL_QUAD(_debug, OID_AUTO, phxagdc_k5, CTLFLAG_RD | CTLFLAG_LOCKED, &gK5, "link-config and plane-capability queries answered");
SYSCTL_INT(_debug, OID_AUTO, phxagdc_last_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gLastRefused, 0, "last AGDC command refused");

// GPUWrangler records each framebuffer's accelerator (the IOAccelIndex IOAcceleratorFamily publishes when it is
// probed) only when it probes the GPU after the PCI device goes quiet. At boot that is before NMXAccel exists, so
// PhxFB stayed "no accelerator" and CoreDisplay composited it in software however the accelerator came up later
// (GPUWranglerGPUGetFramebufferAccelIndex = 0, 2026-10-20). A busy hold on PhxFB keeps the PCI device from going
// quiet; releasing it after the accelerator is up lets the wrangler see it. Write 1 to hold, 0 to release.
// At boot PhxAMDFB starts held, so GPUWrangler publishes the GPU (and WindowServer opens the panel) only after the
// boot daemon has brought the GPU up, started NMXAccel and armed it, and WindowServer composites on the 780M from its
// first start. The daemon releases the hold; a timer releases it anyway so a failed bring-up costs a delay, not a boot.
static IOService *gHoldFB = nullptr;
static int gHold = 0;
static IOLock *gHoldLock = nullptr;
static thread_call_t gHoldTimer = nullptr;
static void holdSet(bool on, const char *why)
{
    if (!gHoldLock || !gHoldFB) return;
    IOLockLock(gHoldLock);
    if (on && !gHold) { gHold = 1; gHoldFB->adjustBusy(1); AGLOG("busy hold on PhxFB (%s): the GPU stays busy", why); }
    else if (!on && gHold) { gHold = 0; gHoldFB->adjustBusy(-1); AGLOG("busy hold released (%s): the GPU may go quiet", why); }
    IOLockUnlock(gHoldLock);
}
static void holdExpired(thread_call_param_t, thread_call_param_t) { holdSet(false, "timeout"); }
void phxHoldInit(IOService *fb, unsigned bootHoldSec)
{
    if (!gHoldLock) gHoldLock = IOLockAlloc();
    gHoldFB = fb;
    if (!bootHoldSec || !gHoldLock) return;
    if (!gHoldTimer) gHoldTimer = thread_call_allocate(holdExpired, nullptr);
    holdSet(true, "boot");
    if (gHoldTimer) {
        uint64_t d; clock_interval_to_deadline(bootHoldSec, kSecondScale, &d);
        thread_call_enter_delayed(gHoldTimer, d);
    }
}
static int phxhold_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gHold;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (!gHoldFB) return ENXIO;
    holdSet(v != 0, "sysctl");
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxfb_hold, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, phxhold_sysctl, "I",
            "1 = hold PhxFB busy (GPUWrangler waits), 0 = release it");

static bool gSysctls = false;
void phxAgdcInstall()
{
    if (gSysctls) return;
    sysctl_register_oid(&sysctl__debug_phxagdc); sysctl_register_oid(&sysctl__debug_phxagdc_cmds);
    sysctl_register_oid(&sysctl__debug_phxagdc_fb_replies); sysctl_register_oid(&sysctl__debug_phxagdc_refused);
    sysctl_register_oid(&sysctl__debug_phxagdc_last_refused); sysctl_register_oid(&sysctl__debug_phxagdc_k5);
    sysctl_register_oid(&sysctl__debug_phxfb_hold);
    gSysctls = true;
}

void phxAgdcRemove()
{
    if (!gSysctls) return;
    sysctl_unregister_oid(&sysctl__debug_phxagdc); sysctl_unregister_oid(&sysctl__debug_phxagdc_cmds);
    sysctl_unregister_oid(&sysctl__debug_phxagdc_fb_replies); sysctl_unregister_oid(&sysctl__debug_phxagdc_refused);
    sysctl_unregister_oid(&sysctl__debug_phxagdc_last_refused); sysctl_unregister_oid(&sysctl__debug_phxagdc_k5);
    sysctl_unregister_oid(&sysctl__debug_phxfb_hold);
    if (gHoldTimer) { thread_call_cancel(gHoldTimer); thread_call_free(gHoldTimer); gHoldTimer = nullptr; }
    holdSet(false, "stop");
    gHoldFB = nullptr;
    gSysctls = false;
    if (gCall) {
        thread_call_cancel(gCall);
        for (int i = 0; gBusy && i < 200; i++) IOSleep(10);
        if (!gBusy) { thread_call_free(gCall); gCall = nullptr; }
    }
    if (gAgdc && !gBusy) { gWant = 0; agdcWork(nullptr, nullptr); }
}
