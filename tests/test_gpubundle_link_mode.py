"""Run the relocation shell script only inside temporary filesystem fixtures."""
import hashlib
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == "darwin", "macOS symlink modes and chmod -h")
class GPUBundleLinkModeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.home = self.root / "relocated"
        self.gpu = self.root / "aliases"
        self.backup = self.root / "backup"
        self.bin = self.root / "bin"
        for path in [self.home, self.gpu, self.backup, self.bin]:
            path.mkdir()
        (self.bin / "codesign").write_text('#!/bin/sh\n[ "$1" = --verify ] && [ "$2" = --strict ] && [ -d "$3" ]\n')
        (self.bin / "codesign").chmod(0o755)
        source = (ROOT / "tools/phxctl/relocate_gpubundle.sh").read_text()
        assignment = "H=/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU; G=/Library/GPUBundles; B=/Library/GPUBundles-backup"
        self.assertEqual(1, source.count(assignment))
        source = source.replace(assignment, f'H="{self.home}"; G="{self.gpu}"; B="{self.backup}"')
        self.script = self.root / "relocate.sh"
        self.script.write_text(source)

    def invoke(self):
        return subprocess.run(
            ["/bin/bash", "-c", 'umask 077; exec /bin/bash "$1"', "fixture", str(self.script)],
            env={"PATH": str(self.bin) + ":/usr/bin:/bin", "LC_ALL": "C"},
            capture_output=True, text=True, timeout=10,
        )

    def fixture(self, parent, name):
        directory = parent / name
        directory.mkdir()
        file = directory / "content"
        file.write_bytes((name + " original contents\n").encode())
        file.chmod(0o640)
        return file

    def link(self, name, mode=0o700, target=None):
        alias = self.gpu / name
        alias.symlink_to(target or self.home / name)
        os.lchmod(alias, mode)
        return alias

    def state(self, file):
        st = file.lstat()
        return (st.st_ino, st.st_uid, st.st_gid, stat.S_IMODE(st.st_mode),
                hashlib.sha256(file.read_bytes()).hexdigest())

    def test_new_metal_alias_is_readable_with_restrictive_umask(self):
        for name in ["NVMTLDriver.bundle", "nvmtl"]:
            self.fixture(self.gpu, name)
        result = self.invoke()
        self.assertEqual(0, result.returncode, result.stderr)
        metal = self.gpu / "NVMTLDriver.bundle"
        self.assertTrue(metal.is_symlink())
        self.assertEqual(str(self.home / metal.name), os.readlink(metal))
        self.assertEqual(0o755, stat.S_IMODE(metal.lstat().st_mode))
        self.assertEqual(0o700, stat.S_IMODE((self.gpu / "nvmtl").lstat().st_mode))
        for name in ["NVMTLDriver.bundle", "nvmtl"]:
            self.assertEqual((self.backup / name / "content").read_bytes(),
                             (self.home / name / "content").read_bytes())
            self.assertEqual(0o640, stat.S_IMODE((self.home / name / "content").stat().st_mode))

    def test_existing_alias_changes_mode_only_and_preserves_targets(self):
        files = [self.fixture(self.home, name) for name in ["NVMTLDriver.bundle", "nvmtl"]]
        before = [self.state(file) for file in files]
        metal, runtime = [self.link(name) for name in ["NVMTLDriver.bundle", "nvmtl"]]
        old = metal.lstat()
        runtime_before = runtime.lstat()
        result = self.invoke()
        self.assertEqual(0, result.returncode, result.stderr)
        new = metal.lstat()
        self.assertEqual((old.st_ino, old.st_uid, old.st_gid), (new.st_ino, new.st_uid, new.st_gid))
        self.assertEqual(0o755, stat.S_IMODE(new.st_mode))
        self.assertEqual(before, [self.state(file) for file in files])
        self.assertEqual((runtime_before.st_ino, runtime_before.st_mode),
                         (runtime.lstat().st_ino, runtime.lstat().st_mode))

    def test_repeated_relocation_keeps_existing_alias_and_target_bytes(self):
        files = [self.fixture(self.home, name) for name in ["NVMTLDriver.bundle", "nvmtl"]]
        for name in ["NVMTLDriver.bundle", "nvmtl"]:
            self.link(name, 0o755)
        before = [self.state(file) for file in files]
        aliases = [(self.gpu / name).lstat().st_ino for name in ["NVMTLDriver.bundle", "nvmtl"]]
        for _ in range(2):
            result = self.invoke()
            self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(before, [self.state(file) for file in files])
        self.assertEqual(aliases, [(self.gpu / name).lstat().st_ino for name in ["NVMTLDriver.bundle", "nvmtl"]])

    def test_unexpected_existing_metal_target_refused_before_relocation(self):
        other = self.root / "other"
        other.mkdir()
        file = self.fixture(other, "NVMTLDriver.bundle")
        before = self.state(file)
        alias = self.link("NVMTLDriver.bundle", target=file.parent)
        runtime = self.fixture(self.gpu, "nvmtl")
        result = self.invoke()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("unexpected link target", result.stderr)
        self.assertEqual(0o700, stat.S_IMODE(alias.lstat().st_mode))
        self.assertEqual(before, self.state(file))
        self.assertTrue(runtime.is_file())
        self.assertFalse((self.home / "nvmtl").exists())


if __name__ == "__main__":
    unittest.main()
