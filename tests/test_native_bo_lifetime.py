"""Execute the actual native BO-free and idle-wait methods against kernel stubs."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class NativeLifetimeTest(unittest.TestCase):
    def test_real_bo_free_preserves_live_resources(self):
        source = (ROOT / 'src/phxbringup/PhxBringup.cpp').read_text()
        code = r'''
#include <cassert>
#include <cstdint>
using IOReturn = int;
constexpr int kIOReturnBadArgument=1, kIOReturnNotFound=2, kIOReturnBusy=3, kIOReturnSuccess=0;
constexpr int kMaxMaps=1;
struct IOExternalMethodArguments { uint64_t scalarInput[1]; };
bool shape(IOExternalMethodArguments *, int, int, int, int) { return true; }
unsigned sleeps;
void IOSleep(unsigned) { sleeps++; }
struct Owner {
    uint64_t retired=0, after=~0ull;
    unsigned unmaps=0;
    uint64_t gpuRetired() { return sleeps >= after ? 7 : retired; }
    void vmUnmap(uint32_t, uint64_t, uint64_t) { unmaps++; }
};
struct IOAccelPhxNativeClient {
    struct Bo { bool host=false; uint32_t domain=2, bits=0; uint64_t size=8; bool live=true; } bo;
    struct Map { bool used=true; uint32_t handle=1; uint64_t va=4096, size=4096; } maps[1];
    Owner *owner;
    uint64_t lastSeq=7, hostUsed=8, gttUsed=8, hiUsed=8, visUsed=8;
    uint32_t vmid=1;
    unsigned releases=0, fences=0;
    Bo *findBo(uint32_t h) { return h==1 && bo.live ? &bo : nullptr; }
    void boRelease(Bo &b) { releases++; b.live=false; }
    void retireFences(uint64_t) { fences++; }
    bool waitIdle();
    IOReturn doBoFree(IOExternalMethodArguments *);
};
constexpr uint32_t PHXN_PLACED_HI_POOL=1;
'''
        code += method(source, 'bool IOAccelPhxNativeClient::waitIdle()')
        code += method(source, 'IOReturn IOAccelPhxNativeClient::doBoFree(')
        code += r'''
int main() {
    Owner owner;
    IOAccelPhxNativeClient client; client.owner=&owner;
    IOExternalMethodArguments args{{1}};
    assert(client.doBoFree(&args)==kIOReturnBusy);
    assert(sleeps==5000 && client.bo.live && client.maps[0].used);
    assert(!client.releases && !owner.unmaps && client.gttUsed==8 && !client.fences);
    owner.after=sleeps+3;
    assert(client.doBoFree(&args)==kIOReturnSuccess);
    assert(!client.bo.live && !client.maps[0].used && client.gttUsed==0);
    assert(client.releases==1 && owner.unmaps==1 && client.fences==1);
    assert(client.doBoFree(&args)==kIOReturnNotFound);
    client.lastSeq=0;
    unsigned before=sleeps;
    assert(client.waitIdle() && sleeps==before);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp)
            (path / 'test.cpp').write_text(code)
            subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)

    def test_close_defers_and_reaps_real_client_methods(self):
        source = (ROOT / 'src/phxbringup/PhxBringup.cpp').read_text()
        code = r'''
#include <cassert>
#include <cstdint>
using IOReturn = int;
constexpr int kIOReturnSuccess=0;
struct Lock { bool held=false; };
void IOLockLock(Lock *l) { assert(!l->held); l->held=true; }
void IOLockUnlock(Lock *l) { assert(l->held); l->held=false; }
struct IOAccelPhxNativeClient;
struct PhxBringup {
    Lock gpuMutex; Lock *gpuLock=&gpuMutex;
    IOAccelPhxNativeClient *retiringClients=nullptr;
    uint64_t retired=0;
    unsigned contexts=0, sessions=0;
    uint64_t gpuRetiredLocked() { assert(gpuLock->held); return retired; }
    bool vmidFree(uint32_t, uint64_t seq) {
        assert(!gpuLock->held);
        if (seq > retired) return false;
        contexts++; return true;
    }
    void releaseSession() { sessions++; }
    void deferClient(IOAccelPhxNativeClient *);
    void reapClients();
};
struct IOAccelPhxNativeClient {
    PhxBringup *owner;
    Lock mutex; Lock *lock=&mutex;
    IOAccelPhxNativeClient *retiringNext=nullptr;
    bool closing=false, sessionClaimed=true;
    uint32_t vmid=1;
    uint64_t lastSeq=7;
    unsigned references=1, frees=0, terminations=0;
    void retain() { references++; }
    void release() { assert(references); references--; }
    void freeAll() { assert(!vmid); frees++; }
    void terminate() { terminations++; }
    bool releaseResources();
    IOReturn clientClose();
};
'''
        for signature in ('bool IOAccelPhxNativeClient::releaseResources()',
                          'void PhxBringup::deferClient(',
                          'void PhxBringup::reapClients()',
                          'IOReturn IOAccelPhxNativeClient::clientClose()'):
            code += method(source, signature)
        code += r'''
int main() {
    PhxBringup owner;
    IOAccelPhxNativeClient a, b; a.owner=b.owner=&owner;
    b.lastSeq=9;
    assert(a.clientClose()==0 && a.closing && a.references==2);
    assert(a.vmid==1 && !a.frees && !owner.contexts && !owner.sessions);
    a.clientClose();
    assert(a.references==2); // duplicate close must not enqueue twice
    b.clientClose();
    owner.reapClients();
    assert(!a.frees && !b.frees);
    owner.retired=7;
    owner.reapClients();
    assert(a.references==1 && a.frees==1 && !a.vmid);
    assert(b.references==2 && !b.frees && b.vmid==1);
    assert(owner.contexts==1 && owner.sessions==1);
    owner.retired=9;
    owner.reapClients();
    assert(!owner.retiringClients && b.references==1 && b.frees==1);
    assert(owner.contexts==2 && owner.sessions==2);
    owner.reapClients();
    assert(a.frees==1 && b.frees==1);
    IOAccelPhxNativeClient c; c.owner=&owner; c.lastSeq=9;
    c.clientClose();
    assert(c.references==1 && c.frees==1 && !owner.retiringClients);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp)
            (path / 'test.cpp').write_text(code)
            subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)

    def test_vmid_release_requires_actual_retirement(self):
        source = (ROOT / 'src/phxbringup/PhxBringup.cpp').read_text()
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
using std::size_t;
void bzero(void *p, size_t n) { std::memset(p, 0, n); }
struct Lock { bool held=false; };
void IOLockLock(Lock *l) { assert(!l->held); l->held=true; }
void IOLockUnlock(Lock *l) { assert(l->held); l->held=false; }
unsigned flushed;
void flushLine(void *) { flushed++; }
constexpr uint32_t kPtChunks=1, kPtChunkPages=1, kFirstUserVmid=8;
struct Buffer { uint8_t bytes[4096]; void *getBytesNoCopy() { return bytes; } };
struct PhxBringup {
    struct VmCtx { Buffer *chunk[1]; uint32_t used=1, hw=8; bool busy=true; } vm[1];
    struct Hw { uint32_t ctx=1; } hwv[1];
    Lock mutex; Lock *gpuLock=&mutex;
    uint64_t retired=0;
    bool stopped=false;
    bool isStopped() const { return stopped; }
    unsigned tlb=0;
    uint64_t gpuRetiredLocked() const { assert(gpuLock->held); return retired; }
    VmCtx *vmCtx(uint32_t handle) { return handle==1 && vm[0].busy ? vm : nullptr; }
    void vmFlushTlb(uint32_t) { assert(gpuLock->held); tlb++; }
    bool vmidFree(uint32_t, uint64_t);
};
'''
        code += method(source, 'bool PhxBringup::vmidFree(')
        code += r'''
int main() {
    Buffer buffer; std::memset(buffer.bytes, 0xab, sizeof(buffer.bytes));
    PhxBringup owner; owner.vm[0].chunk[0]=&buffer;
    assert(!owner.vmidFree(1, 7));
    assert(owner.vm[0].busy && owner.vm[0].hw==8 && owner.hwv[0].ctx==1);
    assert(buffer.bytes[0]==0xab && !owner.tlb && !flushed);
    owner.retired=7;
    owner.stopped=true;
    assert(!owner.vmidFree(1, 7) && owner.vm[0].busy);
    owner.stopped=false;
    assert(owner.vmidFree(1, 7));
    assert(!owner.vm[0].busy && !owner.vm[0].hw && !owner.hwv[0].ctx);
    assert(!buffer.bytes[0] && !buffer.bytes[4095] && owner.tlb==1 && flushed==64);
    assert(owner.vmidFree(1, 7) && owner.tlb==1);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp)
            (path / 'test.cpp').write_text(code)
            subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)

    def test_adopt_preserves_unfinished_queue_history(self):
        source = (ROOT / 'src/phxbringup/PhxBringup.cpp').read_text()
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include "phxbringup/PhxGpuQueue.h"
#include "phxbringup/PhxRange.h"
using IOReturn=int;
constexpr int kIOReturnNotReady=1, kIOReturnBadArgument=2, kIOReturnBusy=3, kIOReturnSuccess=0;
void bzero(void *p, size_t n) { std::memset(p,0,n); }
template<class... T> void IOLog(const char *, T...) {}
struct Lock { bool held=false; };
void IOLockLock(Lock *l) { assert(!l->held); l->held=true; }
void IOLockUnlock(Lock *l) { assert(l->held); l->held=false; }
struct Memory {
    alignas(8) uint8_t bytes[4096]{};
    uintptr_t getVirtualAddress() { return reinterpret_cast<uintptr_t>(bytes); }
    uint64_t getLength() { return sizeof(bytes); }
};
struct PhxBringup {
    using GpuQueue=PhxGpuQueue;
    Memory memory, doorbell;
    Memory *fbAper=&memory, *doorbellMap=&doorbell, *mmio=&memory;
    uint64_t fbMcBase=1, fbAperLen=4096, gSeq=7;
    bool hangCall=true;
    Lock mutex; Lock *gpuLock=&mutex;
    GpuQueue gq[2]{};
    void hdpFlush() {}
    uint64_t queueFence(const GpuQueue &q) {
        uint64_t done; std::memcpy(&done, memory.bytes+q.fenceOff,8); return done;
    }
    IOReturn adoptQueue(const uint64_t in[6]);
};
'''
        code += method(source, 'IOReturn PhxBringup::adoptQueue(')
        code += r'''
int main() {
    PhxBringup owner;
    auto &q=owner.gq[0]; q.ready=true; q.tail=1; q.fseq[0]=7; q.fenceOff=1536;
    uint64_t in[6]={0,256,1024,0,0,1536};
    assert(owner.adoptQueue(in)==kIOReturnBusy);
    assert(q.tail==1 && q.fseq[0]==7 && q.ready && !owner.gpuLock->held);
    uint64_t done=7; std::memcpy(owner.memory.bytes+1536,&done,8);
    assert(owner.adoptQueue(in)==0 && q.ready && !q.tail && q.ringDw==256);
    in[0]=~0ull-255;
    assert(owner.adoptQueue(in)==kIOReturnBadArgument && q.ringDw==256);
    in[0]=0;
    in[2]=~0ull-7;
    assert(owner.adoptQueue(in)==kIOReturnBadArgument);
    in[2]=1024;
    owner.hangCall=false;
    assert(owner.adoptQueue(in)==kIOReturnNotReady && q.ringDw==256);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp)
            (path / 'test.cpp').write_text(code)
            subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'src'),
                            str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
