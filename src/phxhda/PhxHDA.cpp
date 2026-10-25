//
//  PhxHDA.cpp — a Lilu plugin that answers AppleHDA's resource requests in the kernel.
//
//  Tahoe has no AppleHDA, so ours loads from the Auxiliary KC (/Library/Extensions). AppleHDA asks for its layout and
//  platform files with OSKextRequestResource, which kernelmanagerd serves from the kext's bundle; here kernelmanagerd
//  takes no kext outside the system KC into its lookup table ("invalid codeful kernel extension in the restricted
//  lookup table", PhxAccel's too), so the requests for layout7.xml.zlib and Platforms.xml.zlib never came back and
//  AppleHDADriver never registered: no sound (2026-10-25). AppleALC already wraps the callbacks those requests name
//  (AppleHDADriver::layoutLoadCallback, platformLoadCallback) and puts its own layout and platform data in, so the
//  requests are answered here, from a thread call as kernelmanagerd's answers come, with no data of their own.
//  It also points AppleHDA's device strings at /Library/Extensions. -phxhdaoff turns it off.
//
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <libkern/OSKextLib.h>
#include <kern/thread_call.h>

static const char kAppleHDA[] = "com.apple.driver.AppleHDA";
static mach_vm_address_t orgRequestResource;
static volatile SInt32 gTag = 0x50480000;   // 'PH': apart from the tags the kernel gives out

struct PhxHDARequest {
    OSKextRequestResourceCallback callback;
    void *context;
    OSKextRequestTag tag;
    thread_call_t call;
};

static void phxhdaAnswer(thread_call_param_t p0, thread_call_param_t)
{
    auto *r = static_cast<PhxHDARequest *>(p0);
    r->callback(r->tag, kOSKextReturnNotFound, nullptr, 0, r->context);
    DBGLOG("phxhda", "answered request %u", r->tag);
    thread_call_free(r->call);   // a running call is freed once it returns
    IOFree(r, sizeof(*r));
}

static OSReturn phxhdaRequestResource(const char *kextIdentifier, const char *resourceName,
                                      OSKextRequestResourceCallback callback, void *context, OSKextRequestTag *requestTagOut)
{
    if (!kextIdentifier || !callback || strcmp(kextIdentifier, kAppleHDA) != 0)
        return FunctionCast(phxhdaRequestResource, orgRequestResource)(kextIdentifier, resourceName, callback, context, requestTagOut);
    auto *r = static_cast<PhxHDARequest *>(IOMalloc(sizeof(PhxHDARequest)));
    if (!r) return kOSKextReturnNoMemory;
    r->callback = callback; r->context = context; r->tag = (OSKextRequestTag)OSIncrementAtomic(&gTag);
    r->call = thread_call_allocate(phxhdaAnswer, r);
    if (!r->call) { IOFree(r, sizeof(*r)); return kOSKextReturnNoMemory; }
    if (requestTagOut) *requestTagOut = r->tag;
    SYSLOG("phxhda", "%s asks for %s: answered in the kernel (request %u)", kextIdentifier, resourceName ? resourceName : "(null)", r->tag);
    thread_call_enter(r->call);
    return kOSReturnSuccess;
}

// AppleHDA names the bundle with its device strings relative to /System/Library/Extensions, where Tahoe has none, and
// the speakers and microphone showed up as "DevShortNameOut" / "DevShortNameIn" (2026-10-25). Its HAL plug-in path
// is left alone: sound works without it.
static bool phxhdaPublished(void *, void *, IOService *service, IONotifier *)
{
    service->setProperty("IOAudioDeviceLocalizedBundle", "../../../Library/Extensions/AppleHDA.kext");
    SYSLOG("phxhda", "%s: device strings from /Library/Extensions/AppleHDA.kext", service->getName());
    return true;
}

static void phxhdaStart()
{
    if (OSDictionary *match = IOService::serviceMatching("AppleHDADriver")) {
        if (!IOService::addMatchingNotification(gIOFirstPublishNotification, match, phxhdaPublished, nullptr))
            SYSLOG("phxhda", "no publish notification for AppleHDADriver: device names stay unlocalised");
        match->release();
    }
    lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
        KernelPatcher::RouteRequest request("_OSKextRequestResource", phxhdaRequestResource, orgRequestResource);
        if (!patcher.routeMultiple(KernelPatcher::KernelID, &request, 1))
            SYSLOG("phxhda", "OSKextRequestResource not routed (%d): AppleHDA stays without its layout", patcher.getError());
    });
}

static const char *bootargOff[] = { "-phxhdaoff" };
static const char *bootargDebug[] = { "-phxhdadbg" };

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowSafeMode,
    bootargOff, arrsize(bootargOff),
    bootargDebug, arrsize(bootargDebug),
    nullptr, 0,
    KernelVersion::Tahoe,
    KernelVersion::Tahoe,
    phxhdaStart
};
