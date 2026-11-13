"""Run the production scripts with system paths redirected into a temporary tree."""
import os
import re
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CollectionSafety(unittest.TestCase):
    def run_case(self, create=0, inspect=0, backup_failure=False):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            kc = base / 'Library/KernelCollections/AuxiliaryKernelExtensions.kc'
            kc.parent.mkdir(parents=True)
            kc.write_text('working collection')
            (base / 'Library/Extensions/PhxAccel.kext').mkdir(parents=True)
            commands = base / 'bin'
            commands.mkdir()
            def command(name, body):
                file = commands / name
                file.write_text('#!/bin/bash\n' + body)
                file.chmod(0o755)
            command('kmutil', '''if [ "$1" = create ]; then
while [ "$1" != -A ]; do shift; done
printf candidate > "$2"
exit "$CREATE_STATUS"
fi
printf 'com.luuumi.PhxAccel 0.1\\n'
exit "$INSPECT_STATUS"
''')
            if backup_failure:
                command('cp', 'exit 1\n')
            source = (ROOT / 'tools/phxctl/kinst.sh').read_text()
            # Rewrite literals only in this isolated copy; no production path overrides.
            source = re.sub(r'/(Library|System|Users|tmp)/',
                            lambda match: str(base) + match.group(0), source)
            (base / 'tmp').mkdir()
            script = base / 'kinst.sh'
            script.write_text(source)
            env = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'],
                       CREATE_STATUS=str(create), INSPECT_STATUS=str(inspect),
                       BK=str(base / 'backup'))
            result = subprocess.run(['bash', str(script)], env=env, capture_output=True, text=True)
            return result, kc.read_text()

    def test_failed_create_keeps_working_collection(self):
        result, contents = self.run_case(create=1)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(contents, 'working collection')

    def test_failed_inspection_keeps_working_collection(self):
        result, contents = self.run_case(inspect=1)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(contents, 'working collection')

    def test_failed_backup_keeps_working_collection(self):
        result, contents = self.run_case(backup_failure=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(contents, 'working collection')

    def test_success_publishes_candidate(self):
        result, contents = self.run_case()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(contents, 'candidate')


class UninstallSafety(unittest.TestCase):
    def test_failed_rebuild_keeps_collection_and_recovery_files(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            kc = base / 'Library/KernelCollections/AuxiliaryKernelExtensions.kc'
            kc.parent.mkdir(parents=True)
            kc.write_text('working collection')
            gpu = base / 'Users/Shared/gpu'
            gpu.mkdir(parents=True)
            builder = gpu / 'kinst.sh'
            builder.write_text('#!/bin/bash\nexit 1\n')
            builder.chmod(0o755)
            (base / 'Library/LaunchDaemons').mkdir()
            commands = base / 'bin'
            commands.mkdir()
            for name, body in [('id', 'echo 0'), ('nvram', 'exit 0')]:
                file = commands / name
                file.write_text('#!/bin/bash\n' + body + '\n')
                file.chmod(0o755)
            source = (ROOT / 'install/uninstall.sh').read_text()
            source = re.sub(r'/(Library|System|Users|var)/',
                            lambda match: str(base) + match.group(0), source)
            script = base / 'uninstall.sh'
            script.write_text(source)
            result = subprocess.run(['bash', str(script)], capture_output=True, text=True,
                                    env=dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH']))
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(kc.read_text(), 'working collection')
            self.assertTrue(builder.exists())


if __name__ == '__main__':
    unittest.main()
