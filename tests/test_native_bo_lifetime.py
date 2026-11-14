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


if __name__ == '__main__':
    unittest.main()
