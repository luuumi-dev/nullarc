"""Run the production accelerator choice and Vulkan matching against a mixed-GPU registry."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_native_bo_lifetime import method

ROOT = Path(__file__).resolve().parents[1]


class AcceleratorSelection(unittest.TestCase):
    def test_registry_identity_and_vulkan_selection(self):
        source = (ROOT / 'src/nmplugin/nvmtl_vk.c').read_text()
        selection = source[source.index('    VkPhysicalDevice pds[8];'):source.index('    snprintf(g_name,')]
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <pthread.h>
#include <thread>
#include <atomic>
using mach_port_t = unsigned; using io_service_t = unsigned;
constexpr int KERN_SUCCESS=0, kIOMainPortDefault=0, VK_SUCCESS=0, VK_NULL_HANDLE=0;
#define CFSTR(s) s
struct Accel { const char *kind; uint16_t vendor, device; uint64_t identity; };
static Accel registry[]={{"",0,0,0},{"NVAccel",0x10de,0x28e0,1},
                        {"NMXAccel",0x1002,0x1900,2},{"NMXAccel",0x1002,0x1900,3}};
static bool haveAMD=true,haveNV=true;
static bool race=false; static std::atomic<unsigned> arrivals{0};
static pthread_mutex_t g_accelerator_lock = PTHREAD_MUTEX_INITIALIZER;
int IORegistryEntryGetRegistryEntryID(unsigned p,uint64_t *id) {
    if(p>=4) return -1; *id=registry[p].identity; return 0;
}
bool IOObjectConformsTo(unsigned p,const char *kind) { return !strcmp(registry[p].kind,kind); }
const char *IOServiceMatching(const char *s) { return s; }
unsigned IOServiceGetMatchingService(int,const char *s) {
    return !strcmp(s,"NMXAccel") ? (haveAMD?2:0) : (haveNV?1:0);
}
void IOObjectRelease(unsigned) {}
uint16_t nvmtl_pci_id_of(unsigned p,const char *key) {
    if (race && !strcmp(key,"device-id")) {
        arrivals.fetch_add(1);
        while (arrivals.load() != 2) std::this_thread::yield();
    }
    return !strcmp(key,"vendor-id")?registry[p].vendor:registry[p].device;
}
static uint16_t g_vendor,g_pci_device;
static uint64_t g_accelerator_id;
'''
        code += method(source, 'uint16_t nvmtl_vk_set_accelerator(')
        code += method(source, 'uint16_t nvmtl_vk_vendor(')
        code += r'''
using VkPhysicalDevice = unsigned;
struct VkPhysicalDeviceProperties { unsigned vendorID,deviceID; };
static VkPhysicalDeviceProperties devices[9];
static unsigned count=2; static int enumeration=VK_SUCCESS;
static VkPhysicalDevice g_pdev=0; static int g_inst;
int pvkEnumeratePhysicalDevices(int,unsigned *n,VkPhysicalDevice *p) {
    *n=count; for(unsigned i=0;i<count;i++) p[i]=i+1; return enumeration;
}
void pvkGetPhysicalDeviceProperties(unsigned p,VkPhysicalDeviceProperties *props) { *props=devices[p-1]; }
void nvlog(const char *,...) {}
int select_device() { unsigned n=count;
'''
        code += selection + '\nreturn 0; }\n'
        code += r'''
void reset() { g_vendor=g_pci_device=0; g_accelerator_id=0; }
int main() {
    assert(nvmtl_vk_vendor()==0x1002 && g_pci_device==0x1900 && g_accelerator_id==2);
    assert(nvmtl_vk_set_accelerator(2)==0x1002);
    assert(!nvmtl_vk_set_accelerator(1)); // Different vendor.
    assert(!nvmtl_vk_set_accelerator(3)); // Same PCI IDs, different accelerator.
    assert(!nvmtl_vk_set_accelerator(0));
    assert(!nvmtl_vk_set_accelerator(9));
    devices[0]={0x10de,0x28e0}; devices[1]={0x1002,0x1900};
    assert(select_device()==0 && g_pdev==2);
    devices[0]={0x1002,0x1901};
    assert(select_device()==0 && g_pdev==2); // Same vendor is insufficient.
    devices[0]=devices[1];
    assert(select_device()==-1 && !g_pdev); // Same-model ambiguity fails closed.
    devices[0]=devices[1]={0x10de,0x28e0};
    assert(select_device()==-1 && !g_pdev);
    count=9; assert(select_device()==-1); count=0; assert(select_device()==-1);
    count=2; enumeration=-1; assert(select_device()==-1);
    reset(); haveAMD=false;
    assert(nvmtl_vk_vendor()==0x10de && g_accelerator_id==1);
    reset(); haveNV=false;
    assert(nvmtl_vk_vendor()==0 && !g_accelerator_id);
    reset(); race=true; uint16_t a=0,b=0;
    std::thread first([&]{ a=nvmtl_vk_set_accelerator(2); });
    std::thread second([&]{ b=nvmtl_vk_set_accelerator(3); });
    first.join(); second.join();
    assert((a!=0) != (b!=0));
    assert(g_accelerator_id==(a?2:3));
    race=false;
    reset(); haveAMD=true; registry[2].vendor=0x9999;
    assert(nvmtl_vk_vendor()==0 && !g_accelerator_id);
}
'''
        device = (ROOT / 'src/nmplugin/NVMTLDevice.m').read_text()
        self.assertIn('if (!mine || mine != live)', device)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'test.cpp'
            path.write_text(code)
            binary = Path(temp) / 'test'
            subprocess.run(['clang++', '-std=c++17', '-pthread', '-fsanitize=address,undefined', str(path), '-o', str(binary)],
                           check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
