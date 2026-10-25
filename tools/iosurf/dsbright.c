// dsbright: reads or sets the main display's brightness through DisplayServices, the path Control Center's slider and
// the brightness keys take (corebrightnessd), so B-04 can be checked without hands on the slider.
// Usage: dsbright [0..1]
#include <ApplicationServices/ApplicationServices.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    void *ds = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices", RTLD_NOW);
    int (*get)(CGDirectDisplayID, float *) = ds ? dlsym(ds, "DisplayServicesGetBrightness") : NULL;
    int (*set)(CGDirectDisplayID, float) = ds ? dlsym(ds, "DisplayServicesSetBrightness") : NULL;
    if (!get || !set) { fprintf(stderr, "DisplayServices unavailable\n"); return 1; }
    const CGDirectDisplayID d = CGMainDisplayID();
    float b = -1; int r = get(d, &b);
    printf("display %u brightness %.3f (get %d)\n", d, b, r);
    if (argc < 2) return 0;
    r = set(d, (float)atof(argv[1])); get(d, &b);
    printf("set %s -> %d, now %.3f\n", argv[1], r, b);
    return 0;
}
