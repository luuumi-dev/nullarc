// nightshift: turns Night Shift on or off through CoreBrightness's CBBlueLightClient, the call the Control Center
// toggle makes, so what WindowServer does on the switch (B-28) can be measured without hands on the Mac.
// Usage: nightshift [on|off]   (no argument prints the state)
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef struct { BOOL active, enabled, sunSchedulePermitted; int mode; char pad[64]; } BlueLightStatus;

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (!dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness", RTLD_NOW)) {
            fprintf(stderr, "CoreBrightness: %s\n", dlerror()); return 1;
        }
        id client = [NSClassFromString(@"CBBlueLightClient") new];
        if (!client) { fprintf(stderr, "no CBBlueLightClient\n"); return 1; }
        BlueLightStatus st; memset(&st, 0, sizeof st);
        BOOL ok = ((BOOL (*)(id, SEL, BlueLightStatus *))objc_msgSend)(client, NSSelectorFromString(@"getBlueLightStatus:"), &st);
        printf("night shift: %s, active %d, mode %d (status %d)\n", st.enabled ? "on" : "off", st.active, st.mode, ok);
        if (argc < 2) return 0;
        const BOOL on = !strcmp(argv[1], "on");
        ok = ((BOOL (*)(id, SEL, BOOL))objc_msgSend)(client, NSSelectorFromString(@"setEnabled:"), on);
        memset(&st, 0, sizeof st);
        ((BOOL (*)(id, SEL, BlueLightStatus *))objc_msgSend)(client, NSSelectorFromString(@"getBlueLightStatus:"), &st);
        printf("set %s -> %d, now %s\n", argv[1], ok, st.enabled ? "on" : "off");
        return ok ? 0 : 1;
    }
}
