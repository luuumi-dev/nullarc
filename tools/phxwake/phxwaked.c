// phxwaked: runs /usr/local/libexec/phxwake.sh after every system wake. On this machine a system sleep is a
// hibernation (no S3), so the Radeon 780M comes back in its power-on state and the GPU bring-up has to run again
// before the held submissions and the display can continue. Root LaunchDaemon local.phxwake.
//
// A wake from hibernation is a cold boot. When a boot menu (Limine, systemd-boot, GRUB, rEFInd) comes before OpenCore,
// it would show up on every wake. phxresume.py setup records how to skip it once in /usr/local/etc/phxwake-resume:
//   bli <entry id>   Boot Loader Interface: LoaderEntryOneShot = entry, LoaderConfigTimeoutOneShot = "menu-disabled"
//   bootnext <####>  UEFI BootNext = Boot#### (a firmware boot option for the resuming OpenCore)
// It is set as the system goes to sleep (only when PhxSleepPolicy makes that sleep a hibernation) and deleted again
// after the wake and when the sleep is called off; the boot loader or the firmware drops it once used.
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/sysctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static io_connect_t gRoot;
static const char *gScript = "/usr/local/libexec/phxwake.sh";
static const char *gResumeConf = "/usr/local/etc/phxwake-resume";
#define LOADER_GUID "4A67B082-0A4C-41CF-B6C7-440B29BB8C4F"
#define ONESHOT     LOADER_GUID ":LoaderEntryOneShot"
#define TIMEOUT1    LOADER_GUID ":LoaderConfigTimeoutOneShot"
#define BOOTNEXT    "8BE4DF61-93CA-11D2-AA0D-00E098032B8C:BootNext"

enum { kNone, kBli, kBootNext };
static int gMode;
static char gEntry[128];        // bli: entry id
static unsigned gBootNum;       // bootnext: Boot####

static void stamp(const char *what) {
    char t[32]; time_t now = time(NULL); strftime(t, sizeof t, "%F %T", localtime(&now));
    fprintf(stderr, "%s phxwaked: %s\n", t, what); fflush(stderr);
}

static void readConf(void) {
    gMode = kNone;
    FILE *f = fopen(gResumeConf, "r");
    if (!f) return;
    char line[192] = {0}, kind[16] = {0}, arg[128] = {0};
    if (fgets(line, sizeof line, f) && sscanf(line, "%15s %127s", kind, arg) == 2) {
        if (!strcmp(kind, "bli")) { gMode = kBli; strlcpy(gEntry, arg, sizeof gEntry); }
        else if (!strcmp(kind, "bootnext") && sscanf(arg, "%4x", &gBootNum) == 1) gMode = kBootNext;
    }
    fclose(f);
}

static int nvramSetRaw(const char *key, const UInt8 *bytes, CFIndex len) {
    io_registry_entry_t o = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/options");
    if (!o) return -1;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingASCII);
    CFDataRef d = CFDataCreate(NULL, bytes, len);
    kern_return_t kr = IORegistryEntrySetCFProperty(o, k, d);
    if (kr == KERN_SUCCESS) kr = IORegistryEntrySetCFProperty(o, CFSTR("IONVRAM-FORCESYNCNOW-PROPERTY"), k);
    CFRelease(d); CFRelease(k); IOObjectRelease(o);
    return kr == KERN_SUCCESS ? 0 : (int)kr;
}

static void nvramDelete(const char *key) {
    io_registry_entry_t o = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/options");
    if (!o) return;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingASCII);
    IORegistryEntrySetCFProperty(o, CFSTR(kIONVRAMDeletePropertyKey), k);
    CFRelease(k); IOObjectRelease(o);
}

// UTF-16LE with the terminating NUL, as the Boot Loader Interface stores strings
static CFIndex utf16(const char *s, UInt8 *out, CFIndex cap) {
    CFIndex n = 0;
    for (; *s && n + 4 <= cap; s++) { out[n++] = (UInt8)*s; out[n++] = 0; }
    out[n++] = 0; out[n++] = 0;
    return n;
}

static int hibernates(void) {
    int v = 0; size_t l = sizeof v;
    return sysctlbyname("debug.phxsleep_installed", &v, &l, NULL, 0) == 0 && v == 1;
}

static void resumeSet(void) {
    readConf();
    if (gMode == kNone || !hibernates()) return;
    char m[192]; int r;
    if (gMode == kBli) {
        UInt8 b[300]; CFIndex n = utf16(gEntry, b, sizeof b);
        r = nvramSetRaw(ONESHOT, b, n);
        UInt8 z[32]; CFIndex zn = utf16("menu-disabled", z, sizeof z);   // "0" would mean: show the menu, no timeout
        if (!r) r = nvramSetRaw(TIMEOUT1, z, zn);
        snprintf(m, sizeof m, "next boot: entry %s, no menu (LoaderEntryOneShot) %s", gEntry, r ? "FAILED" : "set");
    } else {
        UInt8 le[2] = { (UInt8)(gBootNum & 0xff), (UInt8)(gBootNum >> 8) };
        r = nvramSetRaw(BOOTNEXT, le, 2);
        snprintf(m, sizeof m, "next boot: BootNext=%04X %s", gBootNum, r ? "FAILED" : "set");
    }
    stamp(m);
}

static void resumeClear(const char *why) {
    if (gMode == kNone) return;
    if (gMode == kBli) { nvramDelete(ONESHOT); nvramDelete(TIMEOUT1); }
    else nvramDelete(BOOTNEXT);
    char m[64]; snprintf(m, sizeof m, "one-shot cleared (%s)", why);
    stamp(m);
}

static void power(void *ref, io_service_t svc, natural_t type, void *arg) {
    (void)ref; (void)svc;
    switch (type) {
    case kIOMessageCanSystemSleep:
        IOAllowPowerChange(gRoot, (long)arg);
        break;
    case kIOMessageSystemWillSleep:
        resumeSet();                                    // the kext holds GPU submissions; nothing else to do here
        IOAllowPowerChange(gRoot, (long)arg);
        break;
    case kIOMessageSystemWillNotSleep:
        resumeClear("sleep called off");
        break;
    case kIOMessageSystemHasPoweredOn:
        stamp("system has powered on");
        resumeClear("woke");
        system(gScript);
        break;
    default: break;
    }
}

int main(int argc, char **argv) {
    if (argc > 1) gScript = argv[1];
    IONotificationPortRef port; io_object_t notifier;
    gRoot = IORegisterForSystemPower(NULL, &port, power, &notifier);
    if (!gRoot) { stamp("IORegisterForSystemPower failed"); return 1; }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);
    readConf();
    char m[192];
    if (gMode == kBli) snprintf(m, sizeof m, "waiting for wakes (on hibernation: boot menu entry %s once, no menu)", gEntry);
    else if (gMode == kBootNext) snprintf(m, sizeof m, "waiting for wakes (on hibernation: BootNext=%04X)", gBootNum);
    else snprintf(m, sizeof m, "waiting for wakes");
    stamp(m);
    CFRunLoopRun();
    return 0;
}
