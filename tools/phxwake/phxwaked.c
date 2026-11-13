// phxwaked: runs /usr/local/libexec/phxwake.sh after every system wake. On this machine a system sleep is a
// hibernation (no S3), so the Radeon 780M comes back in its power-on state and the GPU bring-up has to run again
// before the held submissions and the display can continue. Root LaunchDaemon local.phxwake.
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static io_connect_t gRoot;
static const char *gScript = "/usr/local/libexec/phxwake.sh";

static void stamp(const char *what) {
    char t[32]; time_t now = time(NULL); strftime(t, sizeof t, "%F %T", localtime(&now));
    fprintf(stderr, "%s phxwaked: %s\n", t, what); fflush(stderr);
}
static void power(void *ref, io_service_t svc, natural_t type, void *arg) {
    (void)ref; (void)svc;
    switch (type) {
    case kIOMessageCanSystemSleep:
    case kIOMessageSystemWillSleep:
        IOAllowPowerChange(gRoot, (long)arg);          // nothing to do before the sleep: the kext holds submissions
        break;
    case kIOMessageSystemHasPoweredOn:
        stamp("system has powered on");
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
    stamp("waiting for wakes");
    CFRunLoopRun();
    return 0;
}
