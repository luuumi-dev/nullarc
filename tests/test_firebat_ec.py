"""Keep Firebat EC access opt-in and restricted to the captured hardware tuple."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_native_bo_lifetime import method

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'src/phxaccel/accel/nvrm-accel.cpp'


class FirebatECTest(unittest.TestCase):
    def test_boot_opt_in_and_hardware_identity(self):
        source = SOURCE.read_text()
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include "src/amd/phx_platform.h"
bool optedIn = false;
bool PE_parse_boot_argn(const char *name, void *, int) {
    assert(!strcmp(name, "-phxfirebat"));
    return optedIn;
}
struct IOPCIDevice {
    uint16_t vendor=0x1002, device=0x1900, subvendor=0x2014, subdevice=0x8001;
    uint8_t revision=0xba;
    uint16_t configRead16(unsigned offset) {
        switch(offset) {
        case 0: return vendor;
        case 2: return device;
        case 0x2c: return subvendor;
        case 0x2e: return subdevice;
        default: assert(false); return 0;
        }
    }
    uint8_t configRead8(unsigned offset) { assert(offset==8); return revision; }
};
'''
        code += method(source, 'static bool phxFirebatECEnabled(')
        code += r'''
int main() {
    IOPCIDevice pci;
    assert(!phxFirebatECEnabled(&pci));
    optedIn=true;
    assert(!phxFirebatECEnabled(nullptr));
    assert(phxFirebatECEnabled(&pci));
    pci.subdevice=0x8002; assert(!phxFirebatECEnabled(&pci)); pci.subdevice=0x8001;
    pci.subvendor=0x1d05; assert(!phxFirebatECEnabled(&pci)); pci.subvendor=0x2014;
    pci.revision=0xc8; assert(!phxFirebatECEnabled(&pci)); pci.revision=0xba;
    pci.device=0x15bf; assert(!phxFirebatECEnabled(&pci)); pci.device=0x1900;
    pci.vendor=0x10de; assert(!phxFirebatECEnabled(&pci));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / 'ec.cpp'
            binary = Path(directory) / 'ec'
            cpp.write_text(code)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT), str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_every_ec_entry_point_uses_gate_before_access(self):
        source = SOURCE.read_text()
        start = source.index('gAccel = this; gEnableAccel')
        init = source[start:source.index('for (unsigned i = 0; i < NV_CARRIERS', start)]
        self.assertIn('if (phxFirebatECEnabled(fPCI))', init)
        for signature in ('void armFanWorker()', 'void NVAccel::fanWorker(', 'static int firebat_fan_mode_sysctl '):
            body = method(source, signature)
            access = 'ml_io_map(' if 'ml_io_map(' in body else 'thread_call_allocate('
            self.assertLess(body.index('!phxFirebatECEnabled('), body.index(access))
        self.assertEqual(source.count('ml_io_map(0xFE0B0400, 0x100)'), 3)
        self.assertIn('if (NVAccel::gAccel && phxFirebatECEnabled(NVAccel::gAccel->fPCI))\n        sysctl_register_oid(&sysctl__debug_firebat_fan_mode);', source)


if __name__ == '__main__':
    unittest.main()
