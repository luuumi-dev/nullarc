"""Run the actual hang watcher with synthetic reports, time and reboot commands."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FAKE = r'''
import json, os, sys
from pathlib import Path
base=Path(os.environ['PHX_WATCH_TEST'])
state=json.loads((base/'state').read_text())
frames=json.loads((base/'frames').read_text())
name=Path(sys.argv[0]).name
if name=='sleep':
    state['tick']+=1
    (base/'state').write_text(json.dumps(state))
    sys.exit(0 if state['tick'] < len(frames) else 1)
frame=frames[max(0,state['tick'])] if state['tick'] >= 0 else {'time':1000,'reports':[]}
if name=='ls':
    print('\n'.join(frame['reports']))
elif name=='date':
    print(frame['time'] if sys.argv[1:]==['+%s'] else 'test-time')
elif name=='reboot':
    with (base/'reboots').open('a') as f: f.write(str(frame['time'])+'\n')
elif name!='sync':
    raise RuntimeError(name)
'''


class HangWatchTest(unittest.TestCase):
    def run_watch(self, frames, history=()):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            (base / 'state').write_text(json.dumps({'tick': -1}))
            (base / 'frames').write_text(json.dumps(frames))
            (base / 'hang-reboots').write_text(''.join(str(t) + '\n' for t in history))
            (base / 'phxboot-m2').touch()
            commands = base / 'bin'
            commands.mkdir()
            fake = commands / 'fake'
            fake.write_text('#!' + sys.executable + '\n' + FAKE)
            fake.chmod(0o755)
            for command in ('ls', 'sleep', 'date', 'reboot', 'sync'):
                (commands / command).symlink_to(fake)
            # nvram is logged, never invoked on the host.
            nvram = commands / 'nvram'
            nvram.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$PHX_WATCH_TEST/nvram-calls"\n')
            nvram.chmod(0o755)
            source = (ROOT / 'tools/phxctl/phxboot.sh').read_text()
            start = source.index('watch_hangs() {')
            end = source.index('\n}\n', start) + 3
            script = base / 'test.sh'
            script.write_text('G="$PHX_WATCH_TEST"; DR="$G/reports"\nnv_commit() { :; }\n' +
                              source[start:end] + '\nwatch_hangs\n')
            result = subprocess.run(['bash', str(script)], capture_output=True, text=True, timeout=10,
                                    env=dict(os.environ, PATH=str(commands) + ':/usr/bin:/bin',
                                             PHX_WATCH_TEST=str(base), PHX_REBOOT=str(commands / 'reboot')))
            self.assertEqual(result.returncode, 0, result.stderr)
            events = (base / 'reboots').read_text().splitlines() if (base / 'reboots').exists() else []
            return events, (base / 'phxboot-m2').exists()

    def test_distant_reports_do_not_reboot(self):
        events, _ = self.run_watch([
            {'time':1000, 'reports':['WindowServer-a.ips']},
            {'time':1401, 'reports':['WindowServer-a.ips', 'WindowServer-b.ips']}])
        self.assertEqual(events, [])

    def test_second_recent_report_reboots(self):
        events, _ = self.run_watch([
            {'time':1000, 'reports':['WindowServer-a.ips']},
            {'time':1300, 'reports':['WindowServer-a.ips', 'WindowServer-b.ips']}])
        self.assertEqual(events, ['1300'])

    def test_watchdog_spins_count_with_crash_reports(self):
        events, _ = self.run_watch([{'time':1000, 'reports':[
            'WindowServer_a_userspace_watchdog_timeout.spin', 'WindowServer-b.ips', 'Other.ips']}])
        self.assertEqual(events, ['1000'])

    def test_report_rotation_restarts_counter(self):
        events, _ = self.run_watch([
            {'time':1000, 'reports':['WindowServer-a.ips']},
            {'time':1015, 'reports':[]},
            {'time':1030, 'reports':['WindowServer-b.ips']}])
        self.assertEqual(events, [])

    def test_third_reboot_in_hour_disarms_acceleration(self):
        events, armed = self.run_watch([{'time':4000, 'reports':[
            'WindowServer-a.ips', 'WindowServer-b.ips']}], history=[100, 1000, 2000])
        self.assertEqual(events, ['4000'])
        self.assertFalse(armed)

    def test_old_reboot_history_does_not_disarm(self):
        events, armed = self.run_watch([{'time':5000, 'reports':[
            'WindowServer-a.ips', 'WindowServer-b.ips']}], history=[100, 1000])
        self.assertEqual(events, ['5000'])
        self.assertTrue(armed)


if __name__ == '__main__':
    unittest.main()
