"""Execute the actual debug DMA buffer methods under competing clients."""
import pathlib
import subprocess
import tempfile
import unittest
from test_native_bo_lifetime import method

ROOT = pathlib.Path(__file__).resolve().parents[1]


class NativeSysmemTest(unittest.TestCase):
    def test_concurrent_clients_cannot_replace_dma_pages(self):
        source = (ROOT / 'src/phxbringup/PhxBringup.cpp').read_text()
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include "phxbringup/PhxRange.h"
using IOReturn=int;
constexpr int kIOReturnSuccess=0, kIOReturnNotReady=1, kIOReturnBadArgument=2;
constexpr int kIOReturnBusy=3, kIOReturnNoMemory=4;
constexpr int kernel_task=0, kIODirectionInOut=1, kIOMemoryPhysicallyContiguous=2;
void bzero(void *p, size_t n) { std::memset(p,0,n); }
using Lock=std::mutex;
void IOLockLock(Lock *m) { m->lock(); }
void IOLockUnlock(Lock *m) { m->unlock(); }
struct IOBufferMemoryDescriptor {
    alignas(64) uint8_t bytes[4096]{};
    static unsigned allocations, releases;
    static bool fail;
    static IOBufferMemoryDescriptor storage;
    static IOBufferMemoryDescriptor *inTaskWithPhysicalMask(int,int,uint64_t size,uint64_t) {
        allocations++; return fail || size>4096 ? nullptr : &storage;
    }
    void release() { releases++; }
    void *getBytesNoCopy() { return bytes; }
    uint64_t getPhysicalAddress() { return 0x100000; }
    uint64_t getLength() { return sizeof(bytes); }
};
unsigned IOBufferMemoryDescriptor::allocations=0, IOBufferMemoryDescriptor::releases=0;
bool IOBufferMemoryDescriptor::fail=false;
IOBufferMemoryDescriptor IOBufferMemoryDescriptor::storage;
struct PhxBringup {
    Lock mutex; Lock *gpuLock=&mutex;
    IOBufferMemoryDescriptor *sysBuf=nullptr;
    IOReturn sysMem(uint64_t,uint64_t,uint64_t,uint64_t *,uint64_t *);
    IOReturn sysMemLocked(uint64_t,uint64_t,uint64_t,uint64_t *,uint64_t *);
};
'''
        code += method(source, 'IOReturn PhxBringup::sysMem(')
        code += method(source, 'IOReturn PhxBringup::sysMemLocked(')
        code += r'''
int main() {
    PhxBringup owner;
    uint64_t a=9,b=9;
    assert(owner.sysMem(1,0,0,&a,&b)==kIOReturnBadArgument && !a && !b);
    assert(owner.sysMem(99,0,0,&a,&b)==kIOReturnBadArgument);
    IOBufferMemoryDescriptor::fail=true;
    assert(owner.sysMem(1,4096,0,&a,&b)==kIOReturnNoMemory && !owner.sysBuf);
    IOBufferMemoryDescriptor::fail=false;
    int first=-1,second=-1;
    std::thread t1([&] { uint64_t x,y; first=owner.sysMem(1,4096,0,&x,&y); });
    std::thread t2([&] { uint64_t x,y; second=owner.sysMem(1,4096,0,&x,&y); });
    t1.join(); t2.join();
    assert((first==0 && second==kIOReturnBusy) || (second==0 && first==kIOReturnBusy));
    assert(IOBufferMemoryDescriptor::allocations==2 && !IOBufferMemoryDescriptor::releases);
    auto *original=owner.sysBuf;
    assert(owner.sysMem(3,4092,0x12345678,&a,&b)==0);
    assert(owner.sysMem(1,4096,0,&a,&b)==kIOReturnBusy);
    assert(owner.sysMem(1,0,0,&a,&b)==kIOReturnBadArgument);
    assert(owner.sysMem(4,0,0,&a,&b)==kIOReturnBusy && owner.sysBuf==original);
    assert(owner.sysMem(2,4092,0,&a,&b)==0 && b==0x12345678);
    assert(owner.sysMem(2,~0ull-3,0,&a,&b)==kIOReturnBadArgument);
    assert(owner.sysMem(2,4096,0,&a,&b)==kIOReturnBadArgument);
    assert(!IOBufferMemoryDescriptor::releases);
    owner.gpuLock=nullptr;
    assert(owner.sysMem(2,0,0,&a,&b)==kIOReturnNotReady && !a && !b);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp)
            (path / 'test.cpp').write_text(code)
            subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-pthread', '-fsanitize=address,undefined', '-I' + str(ROOT / 'src'),
                            str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
