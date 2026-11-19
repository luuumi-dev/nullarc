// CPU doubles; production functions and completion method are appended by the runner.
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <time.h>
#include <stdio.h>
static const char *programName;
static const char *test_program_name(void) { return programName; }
#define getprogname test_program_name
static _Atomic unsigned waitCalls, clockCalls;
static _Atomic unsigned long long lastElapsed;
static BOOL injectedClock;
static dispatch_semaphore_t started;
static NSArray *queuedHandlers;
static void nvlog(const char *format, ...) {
    if (strstr(format, "deadline expired after")) {
        va_list args; va_start(args, format);
        atomic_store(&lastElapsed, va_arg(args, unsigned long long));
        va_end(args);
    }
}
#define nvtrace(...) ((void)0)
static uint64_t test_clock(clockid_t clock) {
    if (injectedClock) return atomic_fetch_add(&clockCalls, 1) ? 76000000 : 1000000;
    return clock_gettime_nsec_np(clock);
}
static long test_group_wait(dispatch_group_t group, dispatch_time_t deadline) {
    assert(deadline != DISPATCH_TIME_FOREVER);
    atomic_fetch_add(&waitCalls, 1);
    if (started) dispatch_semaphore_signal(started);
    return dispatch_group_wait(group, deadline);
}
@interface NVMTLTexture : NSObject { @public void *_surf; } @end
@implementation NVMTLTexture @end
@interface NVMTLSubmissionPart : NSObject { @public NSSet *surfaces; } @end
@implementation NVMTLSubmissionPart @end
@interface NVMTLCommandBuffer : NSObject {
@public
    NSMutableArray *_parts, *_handlers;
    NSCondition *_completionCondition;
    dispatch_group_t _surfaceCommitGroup;
    BOOL _completionReady;
    id resources, reservation, error;
    int status;
}
- (void)nvmtlFinishCallbacks;
- (void)nvmtlDispatchHandlers:(NSArray *)handlers scheduled:(BOOL)scheduled;
@end
#define clock_gettime_nsec_np test_clock
#define dispatch_group_wait test_group_wait
