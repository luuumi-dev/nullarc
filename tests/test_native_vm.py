"""Exercise real GPUVM mapping and ioctl validation, including rollback failures."""
import pathlib
import subprocess
import tempfile
import unittest
from test_native_bo_lifetime import method

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativeVmTest(unittest.TestCase):
    def test_mapping_boundaries_and_partial_failure(self):
        source = (ROOT / 'src/phxbringup/PhxBringup.cpp').read_text()
        code = r'''
#include <cassert>
#include <cstdint>
#include "phxbringup/PhxRange.h"
#include "phxbringup/PhxNativeABI.h"
using IOReturn=int; using IOByteCount=uint64_t; using addr64_t=uint64_t;
constexpr int kIOReturnSuccess=0, kIOReturnBadArgument=1, kIOReturnNotReady=2;
constexpr int kIOReturnVMError=3, kIOReturnNoMemory=4, kIOReturnNotFound=5;
constexpr int kIOReturnUnsupported=6, kIOReturnNoResources=7, kIOReturnBusy=8;
constexpr int kIOMemoryMapperNone=0, kMaxMaps=1;
constexpr uint64_t kPtAddrMask=0x0000fffffffff000ull, kPteSysFlags=0x77;
constexpr uint64_t kVaMask=(1ull<<48)-1;
struct Lock { bool held=false; };
void IOLockLock(Lock *l) { assert(!l->held); l->held=true; }
void IOLockUnlock(Lock *l) { assert(l->held); l->held=false; }
void flushLine(void *) {}
struct IOMemoryDescriptor {
    int failPage=-1, shortPage=-1;
    unsigned reads=0;
    uint64_t getLength() { return 8192; }
    addr64_t getPhysicalSegment(uint64_t off, IOByteCount *len, int) {
        reads++; *len=(off/4096==static_cast<uint64_t>(shortPage)) ? 2048 : 4096;
        return off/4096==static_cast<uint64_t>(failPage) ? 0 : 0x100000+off;
    }
};
struct PhxBringup {
    struct VmCtx { bool busy=true; unsigned hw=8; } vm[1];
    uint64_t ptes[2]{};
    int failPte=-1;
    unsigned tlbs=0;
    Lock mutex; Lock *gpuLock=&mutex;
    VmCtx *vmCtx(uint32_t h) { return h==1 && vm[0].busy ? vm : nullptr; }
    uint64_t *vmPte(VmCtx &, uint64_t va, bool allocate) {
        assert(gpuLock->held);
        unsigned index=(va-0x10000)/4096; assert(index<2);
        return allocate && index==static_cast<unsigned>(failPte) ? nullptr : ptes+index;
    }
    void vmFlushTlb(unsigned) { assert(gpuLock->held); tlbs++; }
    bool gpuReady() { return true; }
    IOReturn vmUnmap(uint32_t, uint64_t, uint64_t) { return 0; }
    IOReturn vmMap(uint32_t, uint64_t, IOMemoryDescriptor *, uint64_t, uint64_t);
};
struct IOExternalMethodArguments { void *structureInput; };
bool shape(IOExternalMethodArguments *, unsigned, unsigned, unsigned, unsigned) { return true; }
struct IOAccelPhxNativeClient {
    struct Bo { uint64_t size=8192; IOMemoryDescriptor *md; } bo;
    struct Map {
        bool used; uint32_t handle; uint64_t va,size,off; uint32_t flags;
    } maps[1]{};
    PhxBringup *owner;
    unsigned vmid=1;
    Bo *findBo(unsigned h) { return h==1 ? &bo : nullptr; }
    bool ensureVmid() { return true; }
    bool waitIdle() { return true; }
    IOReturn doGemVa(IOExternalMethodArguments *);
};
'''
        code += method(source, 'static bool vaCanonical(')
        code += method(source, 'IOReturn PhxBringup::vmMap(')
        code += method(source, 'IOReturn IOAccelPhxNativeClient::doGemVa(')
        code += r'''
int main() {
    assert(phxRangeFits(0,8192,8192) && phxRangeFits(8192,0,8192));
    assert(!phxRangeFits(~0ull-4095,8192,8192));
    assert(!phxRangeFits(8192,~0ull-4095,8192));
    PhxBringup owner;
    IOMemoryDescriptor md;
    md.failPage=1;
    assert(owner.vmMap(1,0x10000,&md,0,8192)==kIOReturnVMError);
    assert(!owner.ptes[0] && !owner.ptes[1] && owner.tlbs==1);
    md.failPage=-1; owner.failPte=1;
    assert(owner.vmMap(1,0x10000,&md,0,8192)==kIOReturnNoMemory);
    assert(!owner.ptes[0] && !owner.ptes[1] && owner.tlbs==2);
    owner.failPte=-1; md.shortPage=1;
    assert(owner.vmMap(1,0x10000,&md,0,8192)==kIOReturnVMError);
    assert(!owner.ptes[0] && !owner.ptes[1]);
    md.shortPage=-1;
    unsigned reads=md.reads;
    assert(owner.vmMap(1,0x10000,&md,~0ull-4095,8192)==kIOReturnBadArgument);
    assert(owner.vmMap(1,0x10000,&md,0,0)==kIOReturnBadArgument);
    assert(owner.vmMap(1,0x10001,&md,0,4096)==kIOReturnBadArgument);
    assert(md.reads==reads);
    IOAccelPhxNativeClient client; client.owner=&owner; client.bo.md=&md;
    phxn_gem_va input{}; input.handle=1; input.operation=PHX_AMDGPU_VA_OP_MAP;
    input.va_address=0x10000; input.offset_in_bo=8192; input.map_size=~0ull-4095;
    IOExternalMethodArguments args{&input};
    assert(client.doGemVa(&args)==kIOReturnBadArgument && !client.maps[0].used);
    input.offset_in_bo=~0ull-4095; input.map_size=8192;
    assert(client.doGemVa(&args)==kIOReturnBadArgument && !client.maps[0].used);
    input.offset_in_bo=0; input.map_size=8192;
    md.failPage=1;
    assert(client.doGemVa(&args)==kIOReturnVMError && !client.maps[0].used);
    assert(!owner.ptes[0] && !owner.ptes[1]);
    md.failPage=-1;
    assert(client.doGemVa(&args)==0 && client.maps[0].used);
    assert(owner.ptes[0] && owner.ptes[1] && !owner.gpuLock->held);
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
