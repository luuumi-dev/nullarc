//
//  kmod_info.c — XNU kmod glue for PhxHDA.kext: libkmod's _start/_stop call Lilu's plugin entry points.
//
#include <mach/mach_types.h>
#include <libkern/OSKextLib.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t PhxHDA_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t PhxHDA_kern_stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(org.drip.PhxHDA, "1.0.0", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = PhxHDA_kern_start;
__private_extern__ kmod_stop_func_t  *_antimain = PhxHDA_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
