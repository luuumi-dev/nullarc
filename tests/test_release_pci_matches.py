import importlib.util
from pathlib import Path
import plistlib
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('pci_check', ROOT / 'tools/release/check_pci_matches.py')
pci_check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pci_check)


class ReleaseMatches(unittest.TestCase):
    def test_packaged_kexts_must_match_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory)
            for source, destination in (
                ('phxbringup', 'EFI/OC/Kexts/PhxBringup.kext'),
                ('phxaccel', 'Library/Extensions/PhxAccel.kext'),
            ):
                target = package / destination / 'Contents/Info.plist'
                target.parent.mkdir(parents=True)
                shutil.copyfile(ROOT / 'src' / source / 'Info.plist', target)
            pci_check.check(ROOT, package)
            for relative in ('EFI/OC/Kexts/PhxBringup.kext', 'Library/Extensions/PhxAccel.kext'):
                target = package / relative / 'Contents/Info.plist'
                original = target.read_bytes()
                plist = plistlib.loads(original)
                for personality in plist['IOKitPersonalities'].values():
                    if 'IOPCIMatch' in personality:
                        personality['IOPCIMatch'] = '0x19001002'
                target.write_bytes(plistlib.dumps(plist))
                with self.assertRaises(ValueError):
                    pci_check.check(ROOT, package)
                target.write_bytes(original)


if __name__ == '__main__':
    unittest.main()
