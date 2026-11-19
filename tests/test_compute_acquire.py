"""Execute the production submit method with CPU ring, VM and doorbell doubles."""
from pathlib import Path
import subprocess
import tempfile
from test_native_bo_lifetime import method

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / 'src/phxbringup/PhxBringup.cpp').read_text()
    code = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include "phxbringup/PhxGpuQueue.h"
using IOReturn = int;
constexpr int kIOReturnNotReady=1, kIOReturnBadArgument=2, kIOReturnBusy=3, kIOReturnTimeout=4, kIOReturnSuccess=0;
constexpr unsigned kFirstUserVmid=8;
struct Lock { bool held=false; };
void IOLockLock(Lock *p) { assert(!p->held); p->held=true; }
void IOLockUnlock(Lock *p) { assert(p->held); p->held=false; }
void IODelay(unsigned) {}
struct Memory {
    alignas(8) uint64_t words[8192]{};
    uintptr_t getVirtualAddress() { return reinterpret_cast<uintptr_t>(words); }
};
struct PhxBringup {
    using GpuQueue=PhxGpuQueue;
    GpuQueue gq[2]{};
    struct Hw { uint64_t lastSeq=0,binding=7; } hwv[8];
    Lock mutex; Lock *gpuLock=&mutex;
    Memory memory,doorbell; Memory *fbAper=&memory,*doorbellMap=&doorbell;
    uint64_t gSeq=0,fbMcBase=0x8000000000ull;
    bool healthy=true,bindOk=true;
    std::vector<uint32_t> packets;
    PhxBringup() {
        for(unsigned i=0;i<2;i++) { gq[i].ready=true; gq[i].gfx=i; gq[i].ringDw=1024; gq[i].fenceOff=0x1000; }
    }
    bool vmHealthy() { return healthy; }
    bool vmCtx(uint32_t h) { return h==1; }
    uint64_t queueFence(const GpuQueue &) { return 0; }
    unsigned vmBind(uint32_t) { return bindOk ? 8 : 0; }
    void ringPut(GpuQueue &q,unsigned k,uint32_t v) { assert(k==packets.size()); packets.push_back(v); memory.words[(q.wptr+k)&1023]=v; }
    void hdpFlush() {}
    IOReturn gpuSubmit(uint32_t,bool,const uint64_t *,const uint32_t *,uint32_t,uint64_t *);
};
'''
    code += method(source, 'IOReturn PhxBringup::gpuSubmit(')
    code += r'''
int main() {
    const uint32_t acquire[] = {0xc0065800,0,0xffffffff,0x00ffffff,0,0,10,0xc3b1};
    uint64_t va[16]; uint32_t dw[16];
    for(unsigned i=0;i<16;i++) { va[i]=0x800000010000ull+i*0x1000; dw[i]=12+i; }
    unsigned cases=0;
    for(bool gfx : {false,true}) for(unsigned n : {1u,2u,16u}) for(unsigned wrap : {0u,1010u}) {
        PhxBringup p; auto &q=p.gq[gfx]; q.wptr=q.doneEnd=wrap;
        uint64_t seq=999;
        assert(p.gpuSubmit(1,gfx,va,dw,n,&seq)==0);
        const unsigned prefix=gfx?0:8;
        assert(p.packets.size()==prefix+4*n+8 && q.wptr==wrap+prefix+4*n+8);
        if(!gfx) for(unsigned i=0;i<8;i++) assert(p.packets[i]==acquire[i]);
        for(unsigned i=0;i<n;i++) {
            unsigned k=prefix+i*4;
            assert(p.packets[k]==0xc0023f00 && p.packets[k+1]==uint32_t(va[i]));
            assert(p.packets[k+2]==uint32_t((va[i]|0xffff000000000000ull)>>32));
            assert(p.packets[k+3]==(dw[i]|(gfx?0u:1u<<23)|8u<<24));
        }
        assert(p.packets[prefix+4*n]==0xc0064900);
        assert(seq==1 && q.tail==1 && q.fseq[0]==1 && q.fend[0]==q.wptr);
        assert(!p.mutex.held); cases++;
    }
    for(bool gfx : {false,true}) {
        PhxBringup small; small.gq[gfx].ringDw=4+8+(gfx?0:8)+15;
        uint64_t seq=999;
        assert(small.gpuSubmit(1,gfx,va,dw,1,&seq)==kIOReturnBadArgument);
        assert(small.packets.empty() && seq==999 && !small.mutex.held);
        PhxBringup full; full.gq[gfx].wptr=1000;
        assert(full.gpuSubmit(1,gfx,va,dw,1,&seq)==kIOReturnBusy);
        assert(full.packets.empty() && seq==999 && !full.mutex.held);
        PhxBringup bad; bad.bindOk=false;
        assert(bad.gpuSubmit(1,gfx,va,dw,1,&seq)==kIOReturnBusy);
        assert(bad.packets.empty() && !bad.gSeq && !bad.mutex.held);
        cases+=3;
    }
    // A request routed to the only ready engine uses that engine's packet format.
    for(bool gfx : {false,true}) {
        PhxBringup fallback; fallback.gq[gfx].ready=false;
        uint64_t seq=0; assert(!fallback.gpuSubmit(1,gfx,va,dw,1,&seq));
        assert(fallback.packets.size()==(gfx?20u:12u)); cases++;
    }
    assert(cases==20);
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        p=Path(tmp); (p/'test.cpp').write_text(code)
        subprocess.run(['clang++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True,timeout=60)
        subprocess.run([str(p/'test')],check=True,timeout=20)
    print('20 production-submit CPU scenarios passed; no GPU execution')


if __name__ == '__main__':
    main()
