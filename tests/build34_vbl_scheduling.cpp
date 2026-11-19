// Public IOKit timer/gate doubles. These do not validate XNU or a physical GPU.
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
using SInt64 = int64_t;
using OSObject = void;
using IOReturn = int;
using IOFBInterruptProc = void (*)(void *, void *);
constexpr IOReturn kIOReturnSuccess = 0;
constexpr unsigned kIOTimerEventSourceOptionsPriorityHigh = 0;
constexpr unsigned kIOTimerEventSourceOptionsPriorityKernel = 2;
constexpr unsigned kIOTimerEventSourceOptionsAllowReenter = 0x100;
constexpr unsigned kMicrosecondScale = 1000;
struct IOTimerEventSource;
struct IOWorkLoop;
static IOTimerEventSource *allocatedTimer;
static IOWorkLoop *allocatedLoop;
static uint64_t clockNow, highBusyUntil, gPhxVblLastAbs, gPhxVblSeq;
static SInt64 gVblN, gVblLateUsSum, gVblLateUsMax;
static int gVblStaged;
static std::atomic<unsigned> checks{0};
static std::atomic<bool> gateAttempted{false};
static unsigned handlers, depth, maxDepth;
static bool loopFails, timerFails, addFails, injectReentry, agdcRemoved;
static void check(bool value) { assert(value); ++checks; }
static uint64_t mach_absolute_time() { return clockNow; }
static void nanoseconds_to_absolutetime(uint64_t v, uint64_t *out) { *out = v; }
static void absolutetime_to_nanoseconds(uint64_t v, uint64_t *out) { *out = v; }
static SInt64 OSAddAtomic64(SInt64 v, SInt64 *out) { return *out += v; }
static void clock_interval_to_deadline(uint32_t v, unsigned scale, uint64_t *out) {
    *out = clockNow + uint64_t(v) * scale;
}
struct IOWorkLoop {
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    std::recursive_mutex gate;
    bool freed = false;
    static IOWorkLoop *workLoop() {
        return allocatedLoop = loopFails ? nullptr : new IOWorkLoop;
    }
    IOReturn addEventSource(IOTimerEventSource *);
    IOReturn removeEventSource(IOTimerEventSource *);
    IOReturn runAction(Action action, OSObject *owner) {
        gateAttempted = true;
        std::lock_guard<std::recursive_mutex> lock(gate);
        return action(owner, nullptr, nullptr, nullptr, nullptr);
    }
    void release() { freed = true; }
};
static bool dispatch(IOTimerEventSource *);
struct IOTimerEventSource {
    using Action = void (*)(OSObject *, IOTimerEventSource *);
    unsigned options;
    OSObject *owner;
    Action action;
    IOWorkLoop *loop = nullptr;
    uint64_t deadline = 0;
    bool pending = false, enabled = true, running = false, freed = false;
    static IOTimerEventSource *timerEventSource(unsigned options, OSObject *owner, Action action) {
        return allocatedTimer = timerFails ? nullptr : new IOTimerEventSource{options, owner, action};
    }
    IOReturn wakeAtTime(uint64_t when) {
        assert(!freed); pending = true; deadline = when;
        if (running && injectReentry) {
            injectReentry = false; clockNow = deadline;
            check(!dispatch(this));
        }
        return kIOReturnSuccess;
    }
    void disable() { enabled = false; pending = false; }
    void cancelTimeout() { pending = false; }
    void release() { assert(!loop && !running); freed = true; }
};
IOReturn IOWorkLoop::addEventSource(IOTimerEventSource *timer) {
    if (addFails) return -1;
    timer->loop = this;
    return kIOReturnSuccess;
}
IOReturn IOWorkLoop::removeEventSource(IOTimerEventSource *timer) {
    assert(timer->loop == this && !timer->enabled && !timer->pending && !timer->running);
    timer->loop = nullptr;
    return kIOReturnSuccess;
}
static bool dispatch(IOTimerEventSource *timer) {
    if (!timer->loop) return false;
    std::lock_guard<std::recursive_mutex> lock(timer->loop->gate);
    if (!timer->enabled || !timer->pending || timer->deadline > clockNow ||
        (timer->options == kIOTimerEventSourceOptionsPriorityHigh && clockNow < highBusyUntil) ||
        (timer->running && !(timer->options & kIOTimerEventSourceOptionsAllowReenter))) return false;
    timer->pending = false; timer->running = true;
    ++depth; if (depth > maxDepth) maxDepth = depth;
    timer->action(timer->owner, timer);
    --depth; timer->running = false;
    return true;
}
#define OSSafeReleaseNULL(p) do { if (p) { (p)->release(); (p) = nullptr; } } while (0)
static void phxAgdcRemove() {
    assert(!allocatedTimer || allocatedTimer->freed);
    assert(!allocatedLoop || allocatedLoop->freed);
    agdcRemoved = true;
}
struct PhxAMDFB {
    struct Irq { IOFBInterruptProc proc; void *target, *ref; };
    std::vector<Irq> fVbl;
    bool fStopping = false;
    bool fBlDark = false;
    uint64_t fVblNext = 0, fFrameNs = 8333333;
    IOTimerEventSource *fVblTimer = nullptr;
    IOWorkLoop *fVblWorkLoop = nullptr;
    // ACTUAL_VBL_CALLBACK
    // ACTUAL_VBL_RETIRE
    void startTimer() {
        // ACTUAL_VBL_ALLOCATION
    }
    void stopTimer() {
        // ACTUAL_VBL_DRAIN
    }
};
static void handler(void *, void *) { ++handlers; }
static void cleanup() {
    if (allocatedTimer) { check(allocatedTimer->freed); delete allocatedTimer; allocatedTimer = nullptr; }
    if (allocatedLoop) { check(allocatedLoop->freed); delete allocatedLoop; allocatedLoop = nullptr; }
}
static void reset() {
    cleanup(); clockNow = 1000000000; highBusyUntil = 0; gPhxVblLastAbs = gPhxVblSeq = 0;
    gVblN = gVblLateUsSum = gVblLateUsMax = 0; gVblStaged = 1;
    handlers = depth = maxDepth = 0; gateAttempted = false;
    loopFails = timerFails = addFails = injectReentry = agdcRemoved = false;
}
static void release(PhxAMDFB &f) {
    f.stopTimer(); check(f.fStopping && !f.fVblTimer && !f.fVblWorkLoop && agdcRemoved);
    if (allocatedTimer) check(!allocatedTimer->pending && !dispatch(allocatedTimer));
}
static std::mutex heldMutex;
static std::condition_variable heldCondition;
static bool handlerEntered, handlerReleased;
static void heldHandler(void *, void *) {
    std::unique_lock<std::mutex> lock(heldMutex);
    handlerEntered = true; heldCondition.notify_all();
    heldCondition.wait(lock, [] { return handlerReleased; });
}
int main() {
    for (int staged : {0, 1}) {
        reset(); gVblStaged = staged; PhxAMDFB f;
        f.fVbl.push_back({handler, nullptr, nullptr});
        f.startTimer(); auto *timer = f.fVblTimer;
        check(timer && timer->options == kIOTimerEventSourceOptionsPriorityKernel);
        check(!(timer->options & kIOTimerEventSourceOptionsAllowReenter));
        highBusyUntil = clockNow + 300000000;
        IOTimerEventSource control{kIOTimerEventSourceOptionsPriorityHigh, &f, PhxAMDFB::vblFire};
        control.loop = timer->loop; control.pending = true; control.deadline = timer->deadline;
        clockNow = timer->deadline;
        check(!dispatch(&control)); check(dispatch(timer));
        check(gVblN == 1 && gPhxVblSeq == 1 && handlers == 1);
        check(gPhxVblLastAbs == 1008333000 && !gVblLateUsSum);
        check(timer->pending && timer->deadline == f.fVblNext - (staged ? 1500000 : 0));
        if (staged) {
            clockNow = timer->deadline; check(dispatch(timer));
            check(gVblN == 1 && handlers == 1 && timer->deadline == f.fVblNext);
        }
        clockNow = f.fVblNext + 300000000;
        check(dispatch(timer)); check(gVblN == 2 && handlers == 2);
        check(gVblLateUsSum == 300000000 / 1000 && gVblLateUsMax == 300000);
        check(f.fVblNext == clockNow + f.fFrameNs);
        clockNow = f.fVblNext; injectReentry = true;
        check(dispatch(timer)); check(maxDepth == 1 && timer->pending);
        release(f);
    }
    reset(); PhxAMDFB dark; dark.startTimer(); dark.fBlDark = true;
    clockNow = dark.fVblTimer->deadline; check(dispatch(dark.fVblTimer));
    check(!gVblN && !dark.fVblTimer->pending); release(dark);
    reset(); PhxAMDFB stopped; stopped.startTimer(); stopped.fStopping = true;
    clockNow = stopped.fVblTimer->deadline; check(dispatch(stopped.fVblTimer));
    check(!gVblN && !stopped.fVblTimer->pending); release(stopped);
    for (int failure : {0, 1, 2}) {
        reset(); loopFails = failure == 0; timerFails = failure == 1; addFails = failure == 2;
        PhxAMDFB missing; missing.startTimer();
        check(!missing.fVblTimer && !missing.fVblWorkLoop && !missing.fVblNext);
        release(missing);
    }
    // A real host thread holds the modeled gate inside the extracted action.
    // stop() must await it, then cancel its late self-rearm before freeing.
    reset(); PhxAMDFB active; active.fVbl.push_back({heldHandler, nullptr, nullptr});
    active.startTimer(); clockNow = active.fVblTimer->deadline;
    handlerEntered = handlerReleased = false; std::atomic<bool> stopDone{false};
    std::thread firing([&] { check(dispatch(active.fVblTimer)); });
    {
        std::unique_lock<std::mutex> lock(heldMutex);
        heldCondition.wait(lock, [] { return handlerEntered; });
    }
    std::thread stopping([&] { active.stopTimer(); stopDone = true; });
    while (!gateAttempted.load()) std::this_thread::yield();
    check(!stopDone.load());
    {
        std::lock_guard<std::mutex> lock(heldMutex); handlerReleased = true;
    }
    heldCondition.notify_all(); firing.join(); stopping.join();
    check(stopDone && active.fStopping && !active.fVblTimer && !active.fVblWorkLoop);
    check(allocatedTimer->freed && !allocatedTimer->pending && agdcRemoved);
    cleanup();
    std::printf("actual IOKit VBL callback/allocation/gated retirement: %u CPU assertions passed\n", checks.load());
}
