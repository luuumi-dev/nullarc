// PhxSleepPolicy: makes every system sleep a hibernation on a machine whose firmware has no working S3.
//
// Phoenix laptops are Modern Standby only: the firmware hides _S3 (XS3 in the DSDT; seen on the MECHREVO WUJIE15XA). With the name patched back, macOS
// starts sleeping, X86PlatformPlugin's policy picks kIOPMSleepTypeDeepIdle, hibernation setup is skipped and the
// platform enters S3 - from which this firmware never wakes. IOPMrootDomain takes one policy handler, first come first
// served; this kext installs its own before the platform plugin and always answers "hibernate" (image written, power
// off) when hibernation is enabled (pmset hibernatemode 25), and "abort" otherwise, so S3 is never entered.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IONVRAM.h>
#include <IOKit/IOMessage.h>
#include <sys/sysctl.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <kern/thread_call.h>
#include <pexpert/pexpert.h>

struct PolicyVars {            // IOPMSystemSleepPolicyVariables (xnu IOPMPrivate.h)
    uint32_t signature, version; uint64_t currentCapability, highestCapability, sleepFactors;
    uint32_t sleepReason, sleepPhase, hibernateMode, standbyDelay, standbyTimer, poweroffDelay, scheduledAlarms, poweroffTimer;
    uint32_t reserved[49];
};
struct SleepParams {           // IOPMSystemSleepParameters, version 2
    uint16_t version, reserved1; uint32_t sleepType, sleepFlags, ecWakeEvents, ecWakeTimer, ecPoweroffTimer; uint32_t reserved2[10];
} __attribute__((packed));
enum { kTypeAborted = 1, kTypeNormal = 2, kTypeSafeSleep = 3, kTypeHibernate = 4, kHibOn = 1, kHibSleep = 2 };

static int gInstalled, gCalls, gLastType, gLastMode, gAllowS3;
SYSCTL_INT(_debug, OID_AUTO, phxsleep_installed, CTLFLAG_RD, &gInstalled, 0, "1 = our sleep policy handler is the system's, -1 = another one was first");
SYSCTL_INT(_debug, OID_AUTO, phxsleep_calls, CTLFLAG_RD, &gCalls, 0, "");
SYSCTL_INT(_debug, OID_AUTO, phxsleep_last_type, CTLFLAG_RD, &gLastType, 0, "sleep type of the last answer");
SYSCTL_INT(_debug, OID_AUTO, phxsleep_last_mode, CTLFLAG_RD, &gLastMode, 0, "hibernate mode seen last");
SYSCTL_INT(_debug, OID_AUTO, phxsleep_allow_s3, CTLFLAG_RW, &gAllowS3, 0, "1 = answer normal sleep when hibernation is off (hangs on this firmware)");

static IOReturn policy(void *, const PolicyVars *vars, SleepParams *params) {
    if (!vars || !params) return kIOReturnBadArgument;
    bzero(params, sizeof(*params));
    params->version = 2;
    const uint32_t mode = vars->hibernateMode;
    params->sleepType = (mode & kHibOn) ? kTypeHibernate : (gAllowS3 ? kTypeNormal : kTypeAborted);
    gCalls++; gLastType = (int)params->sleepType; gLastMode = (int)mode;
    return kIOReturnSuccess;
}

// The hibernation key. IOHibernateSystemSleep() leaves it in the root domain property "IOHibernateRTCVariables" for
// AppleRTC to store in the extended CMOS a Mac has. Here nothing stores it: OpenCore reports "RTC hibernation is 0,
// NVRAM hibernation is 0 / Not Found", boot.efi cannot read boot-signature / boot-image-key, logs EB|H:FAIL and resets.
// boot.efi and OpenCore also accept the same bytes from the NVRAM variable of that name, so once a sleep starts the
// property is copied to NVRAM and synced (what HibernationFixup does by hooking the kernel; it did not on this macOS).
static int gKeyWrites, gCapMsgs;
SYSCTL_INT(_debug, OID_AUTO, phxsleep_key_writes, CTLFLAG_RD, &gKeyWrites, 0, "hibernation key copies written to NVRAM");
// Called from setPowerState(0). The order inside a sleep is: priority notifications -> willNotifyPowerChildren, where
// IOHibernateSystemSleep() creates the key property -> the root domain's children power down (this service is one) ->
// IOHibernateSystemHasSlept() stores boot-image and syncs NVRAM -> the image is written. So the property exists by
// now and the kernel's own sync a moment later flushes this variable too: only setProperty here. An earlier version
// polled from a thread_call and synced by itself; with it the machine reset or hung while going to sleep.
static void copyKey(void) {
    gCapMsgs++;
    IOPMrootDomain *root = IOService::getPMRootDomain();
    OSObject *o = root ? root->copyProperty("IOHibernateRTCVariables") : nullptr;
    OSData *d = OSDynamicCast(OSData, o);
    IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
    IODTNVRAM *nv = OSDynamicCast(IODTNVRAM, e);
    if (nv) {
        if (d && d->getLength()) {
            const OSSymbol *k = OSSymbol::withCString("IOHibernateRTCVariables");
            OSData *c = OSData::withBytes(d->getBytesNoCopy(), d->getLength());
            if (k && c && nv->setProperty(k, c)) gKeyWrites++;
            if (c) c->release();
            if (k) k->release();
        }
        // what happened, readable after the next boot: "<capability messages> <key length> <key copies>"
        char note[48]; snprintf(note, sizeof note, "%d %u %d", gCapMsgs, d ? d->getLength() : 0, gKeyWrites);
        const OSSymbol *nk = OSSymbol::withCString("phx-sleep-note");
        OSData *nd = OSData::withBytes(note, (unsigned int)strlen(note));
        if (nk && nd) nv->setProperty(nk, nd);
        if (nd) nd->release();
        if (nk) nk->release();
    }
    if (e) e->release();
    if (o) o->release();
}

// After a hibernation the embedded controller has been through a power cycle and is back in its non-ACPI mode: it no
// longer raises its queries, so closing the lid does nothing and the brightness keys are dead (on a MECHREVO WUJIE15XA), while
// keys the keyboard controller reports itself (volume, keyboard backlight) still work. The firmware tells the EC which
// OS runs in EC0._REG (XSEC = OSEC) and again in _WAK, but there only for Arg0 == 4, which macOS does not pass on this
// path. Running the EC's _REG(3, 1) again after every wake puts the EC back into ACPI mode.
static int gEcReg = 0, gEcRegLast = -1;
SYSCTL_INT(_debug, OID_AUTO, phxsleep_ecreg, CTLFLAG_RD, &gEcReg, 0, "EC _REG(3, 1) evaluations (initial power-on and every wake)");
SYSCTL_INT(_debug, OID_AUTO, phxsleep_ecreg_last, CTLFLAG_RD, &gEcRegLast, 0, "result of the last one (0 = ok)");
static void ecRegister(thread_call_param_t, thread_call_param_t) {
    // the EC by its _HID, as AppleACPIEC finds it
    OSDictionary *m = IOService::nameMatching("PNP0C09");
    IOService *svc = m ? IOService::copyMatchingService(m) : nullptr;
    if (m) m->release();
    if (svc && svc->metaCast("IOACPIPlatformDevice")) {
        IOACPIPlatformDevice *ec = static_cast<IOACPIPlatformDevice *>(svc);
        OSObject *args[2] = { OSNumber::withNumber(3ull, 32), OSNumber::withNumber(1ull, 32) };   // EmbeddedControl, connect
        gEcRegLast = (args[0] && args[1]) ? ec->evaluateObject("_REG", nullptr, args, 2) : -2;
        if (args[0]) args[0]->release();
        if (args[1]) args[1]->release();
        gEcReg++;
        IOLog("PhxSleepPolicy: %s._REG(3, 1) -> 0x%x\n", ec->getName(), gEcRegLast);
    } else IOLog("PhxSleepPolicy: no embedded controller (PNP0C09) found, nothing re-registered\n");
    if (svc) svc->release();
}
// debug.phxsleep_ecreg_now: write 1 to run it by hand
static int ecRegNow(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
    int v = 0; int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v == 1) ecRegister(nullptr, nullptr);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, phxsleep_ecreg_now, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, ecRegNow, "I", "write 1: evaluate the EC's _REG(3, 1) now");
static thread_call_t gEcCall;

// At the initial power-on too. ACPICA runs _REG(EmbeddedControl, 1) only for the OperationRegions of that space, and the
// interpreter in AppleACPIPlatform does not run the "orphan" EC _REG that newer ACPICA (Linux) and Windows run when an EC
// has none: on an ACEMAGIC RX16 every EC field lives in a SystemMemory window (ERAX), so _REG never runs under macOS,
// OSFG ("an ACPI OS is present") stays 0, the EC handles the lid by itself and never raises _Q0C/_Q0D. Once the ACPI EC
// driver is up, run it once. Where macOS did run it, this repeats the same _REG(3, 1) that a re-registration of the
// region would. The boot-arg -phxsleepnoecreg leaves the initial power-on alone (the wake path stays).
static IONotifier *gEcNotifier;
static bool ecDriverUp(void *, void *, IOService *, IONotifier *n) {
    static bool done = false;
    if (!done && gEcCall) {
        done = true;
        uint64_t d; clock_interval_to_deadline(1000, kMillisecondScale, &d);
        thread_call_enter_delayed(gEcCall, d);
    }
    return true;
}

class PhxSleepPolicy : public IOService {
    OSDeclareDefaultStructors(PhxSleepPolicy)
public:
    bool start(IOService *provider) override;
    IOReturn setPowerState(unsigned long ordinal, IOService *whatDevice) override;
};
OSDefineMetaClassAndStructors(PhxSleepPolicy, IOService)

bool PhxSleepPolicy::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    sysctl_register_oid(&sysctl__debug_phxsleep_installed); sysctl_register_oid(&sysctl__debug_phxsleep_calls);
    sysctl_register_oid(&sysctl__debug_phxsleep_last_type); sysctl_register_oid(&sysctl__debug_phxsleep_last_mode);
    sysctl_register_oid(&sysctl__debug_phxsleep_allow_s3);
    IOPMrootDomain *root = getPMRootDomain();
    const OSSymbol *fn = OSSymbol::withCString("IOPMInstallSystemSleepPolicyHandler");
    IOReturn r = (root && fn) ? root->callPlatformFunction(fn, false, (void *)&policy, this, nullptr, nullptr) : kIOReturnNotReady;
    if (fn) fn->release();
    gInstalled = r == kIOReturnSuccess ? 1 : -1;
    IOLog("PhxSleepPolicy: install -> 0x%x (%s)\n", r, r == kIOReturnSuccess ? "ours" : "another handler was first");
    sysctl_register_oid(&sysctl__debug_phxsleep_key_writes);
    sysctl_register_oid(&sysctl__debug_phxsleep_ecreg); sysctl_register_oid(&sysctl__debug_phxsleep_ecreg_last); sysctl_register_oid(&sysctl__debug_phxsleep_ecreg_now);
    gEcCall = thread_call_allocate(ecRegister, nullptr);
    char flag = 0;
    if (gEcCall && !PE_parse_boot_argn("-phxsleepnoecreg", &flag, sizeof flag)) {
        OSDictionary *m = IOService::serviceMatching("AppleACPIEC");
        if (m) {
            gEcNotifier = addMatchingNotification(gIOFirstPublishNotification, m, ecDriverUp, nullptr);
            m->release();
        }
    }
    static IOPMPowerState states[2] = {
        { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { kIOPMPowerStateVersion1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 } };
    PMinit(); provider->joinPMtree(this); registerPowerDriver(this, states, 2);
    registerService();
    return true;
}

IOReturn PhxSleepPolicy::setPowerState(unsigned long ordinal, IOService *) {
    static bool slept = false;   // the initial power-on is covered by ecDriverUp()
    if (ordinal == 0) { slept = true; copyKey(); }
    else if (gEcCall && slept) {   // two seconds after power-on: the ACPI EC driver is awake by then
        uint64_t d; clock_interval_to_deadline(2000, kMillisecondScale, &d);
        thread_call_enter_delayed(gEcCall, d);
    }
    return kIOPMAckImplied;
}
