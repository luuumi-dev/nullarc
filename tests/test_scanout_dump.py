"""The real scanout dumper must not report incomplete captures as success."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_phxctl_cli import IOKIT, ROOT


class ScanoutDump(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.base = Path(cls.temp.name)
        (cls.base / 'IOKit').mkdir()
        header = IOKIT.replace('if (buf && sz) {',
                              'if (buf && sz) { if (getenv("SHORT_READ")) *sz = 0;')
        (cls.base / 'IOKit/IOKitLib.h').write_text(header)
        cls.binary = cls.base / 'scanout_dump'
        subprocess.run([shutil.which('clang') or 'cc', '-Wall', '-Wextra', '-Werror',
                        '-I', str(cls.base), str(ROOT / 'tools/iosurf/scanout_dump.c'),
                        '-o', str(cls.binary)], check=True, capture_output=True)

    def run_dump(self, args, **env):
        return subprocess.run([str(self.binary), *map(str, args)],
                              env=dict(os.environ, **env), capture_output=True, timeout=5)

    def test_geometry(self):
        for geometry in [('0', '2', '16'), ('4', '-1', '16'), ('4x', '2', '16'),
                         ('5', '2', '16'), ('4', '2', '17'), ('4294967296', '2', '16'),
                         ('4',)]:
            with self.subTest(geometry=geometry):
                self.assertEqual(self.run_dump([self.base / 'bad', *geometry]).returncode, 2)

    def test_read_failures(self):
        for env in ({'SHORT_READ': '1'}, {'FAIL_SEL': '102'}, {'FAIL_SEL': '0'}):
            with self.subTest(env=env):
                self.assertEqual(self.run_dump([self.base / 'fail', '4', '2', '16'], **env).returncode, 1)

    def test_output_failures(self):
        for path in (self.base / 'absent/file', Path('/dev/full')):
            with self.subTest(path=path):
                self.assertEqual(self.run_dump([path, '4', '2', '16']).returncode, 1)

    def test_success_omits_padding(self):
        path = self.base / 'good'
        self.assertEqual(self.run_dump([path, '257', '2', '1040']).returncode, 0)
        self.assertEqual(path.stat().st_size, 257 * 2 * 4)


if __name__ == '__main__':
    unittest.main()
