"""Execute native bind/submit/ioctl methods and verify failure has no publication."""
import pathlib
import subprocess
import tempfile
import unittest
from test_native_bo_lifetime import method

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativeSubmitTest(unittest.TestCase):
    def test_bind_failure_propagates_without_publishing(self):
        source = (ROOT / 'src/phxbringup/PhxBringup.cpp').read_text()
        code = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include "phxbringup/PhxNativeABI.h"
#include "phxbringup/PhxGpuQueue.h"
#include "phxbringup/PhxPendingFences.h"
using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnBusy=2,kIOReturnTimeout=3;
constexpr int kIOReturnBadArgument=4,kIOReturnNotFound=5,kIOReturnUnsupported=6;
constexpr int kIOReturnNoResources=7,kIOReturnNoMemory=8;
constexpr unsigned kFirstUserVmid=8,kNumUserVmids=8,kMaxCtx=64;
constexpr int kMaxMaps=1;
constexpr unsigned kRegCtx0BaseLo=1,kRegCtx0Cntl=100,kRegCtx0StartLo=200,kRegCtx0EndLo=300;
constexpr uint64_t kVaMask=(1ull<<48)-1;
struct Lock { bool held=false; };
void IOLockLock(Lock *l) { assert(!l->held); l->held=true; }
void IOLockUnlock(Lock *l) { assert(l->held); l->held=false; }
void IODelay(unsigned) {}
void bcopy(const void *s,void *d,size_t n) { std::memmove(d,s,n); }
template<class... T> void IOLog(const char *,T...) {}
unsigned allocations=0; bool failAllocation=false;
void *IOMallocZero(size_t n) { if(failAllocation)return nullptr; allocations++; return std::calloc(1,n); }
void IOFree(void *p,size_t) { assert(allocations); allocations--; std::free(p); }
class IOMemoryDescriptor { public: unsigned refs=1; void retain(){refs++;} void release(){refs--;} };
struct Memory { alignas(8) uint64_t words[8192]{}; uintptr_t getVirtualAddress(){return reinterpret_cast<uintptr_t>(words);} };
struct PhxBringup {
    using GpuQueue=PhxGpuQueue;
    struct VmCtx { bool busy=true; unsigned hw=0; uint64_t phys=0x100000; } vm[kMaxCtx];
    struct HwVmid { unsigned ctx=0; uint64_t lastSeq=0,lastUse=0,binding=0; } hwv[8];
    GpuQueue gq[2]{};
    Lock mutex; Lock *gpuLock=&mutex;
    uint64_t hwTick=0,gSeq=0,fbMcBase=0x8000000000ull;
    bool fault=false,flushOk=false;
    Memory ring,doorbell; Memory *fbAper=&ring,*doorbellMap=&doorbell;
    unsigned packetWrites=0,registerWrites=0;
    PhxBringup(){gq[0].ready=true;gq[0].ringDw=1024;}
    bool vmHealthy()const{return !fault;}
    bool gpuReady()const{return true;}
    bool debugEnabled()const{return false;}
    uint64_t gpuRetiredLocked()const{return 0;}
    uint64_t gpuRetired()const{return 0;}
    uint64_t queueFence(const GpuQueue &)const{return 0;}
    VmCtx *vmCtx(unsigned h){return h && h<=kMaxCtx ? &vm[h-1] : nullptr;}
    void wreg(unsigned,unsigned){registerWrites++;}
    bool vmFlushTlb(unsigned){if(!flushOk)fault=true;return flushOk;}
    void ringPut(GpuQueue &,unsigned,uint32_t){packetWrites++;}
    void hdpFlush(){}
    uint32_t vmBind(uint32_t);
    IOReturn gpuSubmit(uint32_t,bool,const uint64_t *,const uint32_t *,uint32_t,uint64_t *);
};
struct IOExternalMethodArguments {
    uint32_t scalarInputCount=0,scalarOutputCount=1;
    size_t structureOutputSize=0,structureInputSize=0;
    void *structureInputDescriptor=nullptr;
    const void *structureInput=nullptr;
    uint64_t scalarOutput[1]{999};
};
struct IOAccelPhxNativeClient {
    struct Bo { IOMemoryDescriptor *md; uint64_t size=8; uint8_t *kva; } bo;
    struct Map { bool used=false; uint64_t va=0,size=0,off=0; unsigned handle=0; } maps[1];
    PhxBringup *owner;
    bool ctx[PHXN_MAX_CTX+1]{};
    uint64_t emitted=0,lastSeq=0;
    unsigned vmid=1;
    PhxPendingFences pendingFences;
    IOAccelPhxNativeClient(){ctx[1]=true;}
    Bo *findBo(unsigned h){return h==1 ? &bo : nullptr;}
    bool ibCovered(uint64_t,uint64_t){return true;}
    bool ensureVmid(){return true;}
    void flushBos(){}
    void retireFences(uint64_t){}
    IOReturn doSubmit(IOExternalMethodArguments *);
};
'''
        for signature in ('static bool vaCanonical(', 'uint32_t PhxBringup::vmBind(',
                          'IOReturn PhxBringup::gpuSubmit(', 'IOReturn IOAccelPhxNativeClient::doSubmit('):
            code += method(source, signature)
        code += r'''
int main() {
    struct Packet { phxn_cs_in h; phxn_cs_ib ib; } packet{};
    packet.h.abi=PHXN_ABI_VERSION;packet.h.num_ibs=1;packet.h.ctx_id=1;
    packet.h.flags=PHXN_CS_HAS_FENCE;packet.h.fence_handle=1;
    packet.ib.ip_type=PHX_AMDGPU_HW_IP_COMPUTE;packet.ib.va_start=0x10000;packet.ib.ib_bytes=4;
    IOMemoryDescriptor md;uint64_t destination=0;
    PhxBringup owner;
    IOAccelPhxNativeClient client;client.owner=&owner;client.bo.md=&md;
    client.bo.kva=reinterpret_cast<uint8_t *>(&destination);
    IOExternalMethodArguments args;args.structureInput=&packet;args.structureInputSize=sizeof(packet);
    assert(client.doSubmit(&args)==kIOReturnTimeout);
    assert(owner.fault && !owner.gSeq && !owner.gq[0].tail && !owner.packetWrites);
    assert(!owner.doorbell.words[0] && !owner.gpuLock->held && owner.registerWrites);
    assert(!allocations && md.refs==1 && !client.pendingFences.head);
    assert(!client.emitted && !client.lastSeq && args.scalarOutput[0]==999);
    assert(client.doSubmit(&args)==kIOReturnNotReady && !allocations);
    PhxBringup busy;
    for(auto &h:busy.hwv){h.ctx=1;h.lastSeq=100;}
    client.owner=&busy;
    assert(client.doSubmit(&args)==kIOReturnBusy && !busy.fault && !allocations);
    assert(!busy.packetWrites && !busy.gSeq);
    PhxBringup oom;client.owner=&oom;failAllocation=true;
    assert(client.doSubmit(&args)==kIOReturnNoMemory && !oom.registerWrites && !allocations);
    failAllocation=false;
    PhxBringup success;success.flushOk=true;client.owner=&success;
    assert(client.doSubmit(&args)==0 && success.gSeq==1 && success.packetWrites);
    assert(client.emitted==1 && client.lastSeq==1 && args.scalarOutput[0]==1);
    assert(allocations==1 && md.refs==2 && client.pendingFences.head);
    auto *f=client.pendingFences.take(1);f->memory->release();IOFree(f,sizeof(*f));
    assert(!allocations && md.refs==1 && !client.pendingFences.head);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp)
            (path / 'test.cpp').write_text(code)
            subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I' + str(ROOT / 'src'),
                            str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
