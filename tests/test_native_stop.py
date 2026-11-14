"""Run production lifecycle/stop methods with real read/write lock concurrency."""
import pathlib
import subprocess
import tempfile
import unittest

from test_native_bo_lifetime import method

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativeStopTest(unittest.TestCase):
    def test_stop_waits_for_calls_and_preserves_dma_resources(self):
        source = (ROOT / 'src/phxbringup/PhxBringup.cpp').read_text()
        code = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <thread>
struct IORWLock { std::shared_mutex mutex; std::atomic<bool> writerWaiting{false}; };
thread_local bool writer=false;
void IORWLockRead(IORWLock *l) { l->mutex.lock_shared(); }
void IORWLockWrite(IORWLock *l) { l->writerWaiting=true; l->mutex.lock(); writer=true; }
void IORWLockUnlock(IORWLock *l) {
    if (writer) { writer=false; l->mutex.unlock(); } else l->mutex.unlock_shared();
}
unsigned lockFrees=0;
struct Lock {};
void IOLockFree(Lock *) { lockFrees++; }
unsigned cancelCalls=0, callFrees=0;
void thread_call_cancel_wait(void *) { cancelCalls++; }
void thread_call_free(void *) { callFrees++; }
int gSysctlUp=0;
int sysctl__debug_phxbos_flush, sysctl__debug_phxbos_flush_bytes;
int sysctl__debug_phx_hang_ms, sysctl__debug_phx_hang_kills;
void sysctl_unregister_oid(int *) {}
template<class... T> void IOLog(const char *, T...) {}
struct IOService { unsigned superStops=0; void stop(IOService *) { superStops++; } };
struct Resource { unsigned releases=0; void release() { releases++; } };
using IOBufferMemoryDescriptor=Resource;
struct PhxBringup : IOService {
    IORWLock lifecycle; IORWLock *lifecycleLock=&lifecycle;
    bool stopped=false;
    bool isStopped() const { return __atomic_load_n(&stopped,__ATOMIC_ACQUIRE); }
    Lock gpuMutex, smuMutex; Lock *gpuLock=&gpuMutex, *smuLock=&smuMutex;
    Resource resource;
    Resource *fbAper=&resource, *sysBuf=nullptr, *doorbellMap=&resource;
    Resource *mmio=&resource, *pci=&resource;
    struct Queue { bool ready=false; } gq[2];
    struct VmCtx { Resource *chunk[1]{}; } vm[1];
    struct HwVmid { unsigned ctx=0; } hwv[1];
    void *hangCall=this;
    bool hangStop=false;
    unsigned retains=0, clears=0, pmStops=0;
    void retain() { retains++; }
    void clearRing() { clears++; }
    void PMstop() { pmStops++; }
    bool beginAccess();
    void endAccess();
    void stop(IOService *);
};
'''
        for signature in ('bool PhxBringup::beginAccess()',
                          'void PhxBringup::endAccess()',
                          'void PhxBringup::stop(IOService *provider)'):
            code += method(source, signature)
        code += r'''
int main() {
    PhxBringup active;
    active.gq[0].ready=true; active.vm[0].chunk[0]=&active.resource;
    assert(active.beginAccess());
    std::thread stopper([&] { active.stop(nullptr); });
    while (!active.lifecycle.writerWaiting) std::this_thread::yield();
    assert(!active.isStopped() && !active.resource.releases && !active.retains);
    active.endAccess();
    stopper.join();
    assert(active.isStopped() && !active.beginAccess());
    assert(active.retains==1 && !active.resource.releases && !lockFrees);
    assert(active.fbAper && active.mmio && active.gpuLock && active.vm[0].chunk[0]);
    assert(!active.hangCall && active.hangStop && active.superStops==1);
    assert(cancelCalls==1 && callFrees==1);
    active.stop(nullptr);
    assert(active.retains==1 && active.superStops==1);
    PhxBringup debug;
    debug.sysBuf=&debug.resource;
    debug.stop(nullptr);
    assert(debug.retains==1 && !debug.resource.releases && debug.sysBuf);
    PhxBringup idle;
    idle.vm[0].chunk[0]=&idle.resource;
    idle.stop(nullptr);
    assert(!idle.retains && idle.resource.releases==5 && lockFrees==2);
    assert(!idle.fbAper && !idle.mmio && !idle.gpuLock);
    assert(!idle.beginAccess() && idle.superStops==1);
    PhxBringup allocationFailure;
    allocationFailure.lifecycleLock=nullptr;
    assert(!allocationFailure.beginAccess());
    allocationFailure.stop(nullptr);
    assert(allocationFailure.superStops==1);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp)
            (path / 'test.cpp').write_text(code)
            subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-pthread', str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
