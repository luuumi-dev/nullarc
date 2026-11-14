"""Exercise the watcher with fake network commands and no real sleeps or resets."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

FAKE = r'''
import json
import os
from pathlib import Path
import sys

base = Path(os.environ['WATCH_TEST'])
state = json.loads((base / 'state').read_text())
frames = json.loads((base / 'frames').read_text())
frame = frames[state['tick']]
name = Path(sys.argv[0]).name
args = sys.argv[1:]
with (base / 'events').open('a') as log:
    log.write(json.dumps([state['tick'], name, args]) + '\n')
status = 0
if name == 'ifconfig':
    interface = frame.get('after_reset', 'en7') if state['reset'] else 'en7'
    if frame.get('present', True) and interface:
        if args == ['-a'] or args == [interface]:
            print(interface + ': flags=8863<UP,BROADCAST,RUNNING>')
            print('\tether 98:03:8e:79:ea:42')
            if frame.get('address'):
                print('\tinet ' + frame['address'] + ' netmask 0xffffff00')
            print('\tstatus: ' + ('active' if frame.get('active', True) else 'inactive'))
elif name == 'route':
    if frame.get('gateway', True):
        print('    gateway: 192.0.2.1')
        print('    interface: ' + frame.get('route_interface', frame.get('after_reset', 'en7') if state['reset'] else 'en7'))
    else:
        status = 1
elif name == 'ping':
    status = 0 if frame.get('ping', True) else 1
elif name == 'networksetup':
    print('Hardware Port: USB Ethernet\nDevice: en7\nHardware Port: Wi-Fi\nDevice: en0')
elif name == 'ue200reset':
    state['reset'] = True
elif name == 'logger':
    if args == ['-t', 'ue200watch']:
        sys.stdin.read()
elif name == 'sleep' and args == ['15']:
    state['tick'] += 1
    state['reset'] = False
    if state['tick'] == len(frames):
        status = 99
elif name not in ('sleep', 'ipconfig'):
    raise RuntimeError('unexpected command: ' + name)
if name in ('ue200reset', 'sleep'):
    (base / 'state').write_text(json.dumps(state))
sys.exit(status)
'''


class UE200Watcher(unittest.TestCase):
    def run_watcher(self, frames):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / 'frames').write_text(json.dumps(frames))
            (base / 'state').write_text(json.dumps({'tick': 0, 'reset': False}))
            commands = base / 'bin'
            commands.mkdir()
            fake = commands / 'fake'
            fake.write_text('#!' + sys.executable + '\n' + FAKE)
            fake.chmod(0o755)
            for name in ('ifconfig', 'ipconfig', 'route', 'ping', 'networksetup',
                         'ue200reset', 'logger', 'sleep'):
                (commands / name).symlink_to(fake)
            # Redirect only the absolute reset path in an isolated script copy.
            source = (ROOT / 'tools/ue200/ue200watch.sh').read_text()
            script = base / 'watch.sh'
            script.write_text(source.replace('/usr/local/bin/ue200reset',
                                             str(commands / 'ue200reset')))
            startup = base / 'startup.sh'
            startup.write_text('sleep() { command sleep "$@"; '
                               'if [ "$?" -eq 99 ]; then exit 0; fi; }\n')
            result = subprocess.run(
                ['bash', str(script)], capture_output=True, text=True, timeout=15,
                env=dict(os.environ, PATH=str(commands) + ':/usr/bin:/bin',
                         BASH_ENV=str(startup), WATCH_TEST=str(base)))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return [json.loads(line) for line in (base / 'events').read_text().splitlines()]

    @staticmethod
    def actions(events, command):
        return [(tick, args) for tick, name, args in events if name == command]

    def test_no_lease_retries_then_resets_even_with_working_gateway(self):
        for address in (None, '169.254.10.20'):
            with self.subTest(address=address):
                events = self.run_watcher([{'address': address}] * 8)
                self.assertEqual(self.actions(events, 'ipconfig'),
                                 [(3, ['set', 'en7', 'DHCP']), (7, ['set', 'en7', 'DHCP'])])
                self.assertEqual(self.actions(events, 'ue200reset'), [(7, [])])

    def test_valid_lease_never_triggers_recovery(self):
        events = self.run_watcher([{'address': '192.0.2.20'}] * 8)
        self.assertEqual(self.actions(events, 'ipconfig'), [])
        self.assertEqual(self.actions(events, 'ue200reset'), [])

    def test_link_interruption_restarts_no_lease_counter(self):
        for interruption in ({'active': False}, {'present': False}):
            with self.subTest(interruption=interruption):
                events = self.run_watcher([{}] * 3 + [interruption] + [{}] * 4)
                self.assertEqual(self.actions(events, 'ipconfig'), [(7, ['set', 'en7', 'DHCP'])])
                self.assertEqual(self.actions(events, 'ue200reset'), [])

    def test_dhcp_uses_interface_rediscovered_after_reset(self):
        events = self.run_watcher([{'after_reset': 'en9'}] * 8)
        self.assertEqual(self.actions(events, 'ipconfig'),
                         [(3, ['set', 'en7', 'DHCP']), (7, ['set', 'en9', 'DHCP'])])

    def test_no_dhcp_on_stale_interface_if_reset_device_is_missing(self):
        events = self.run_watcher([{'after_reset': ''}] * 8)
        self.assertEqual(self.actions(events, 'ipconfig'), [(3, ['set', 'en7', 'DHCP'])])

    def test_two_recovery_branches_do_not_reset_twice_in_one_iteration(self):
        events = self.run_watcher([{}] * 4 + [{'ping': False}] * 4)
        self.assertEqual(self.actions(events, 'ue200reset'), [(7, [])])

    def test_gateway_failure_renews_only_ue200(self):
        for failure in ({'ping': False}, {'gateway': False}):
            with self.subTest(failure=failure):
                events = self.run_watcher([dict(failure, address='192.0.2.20')] * 4)
                self.assertEqual(self.actions(events, 'ue200reset'), [(3, [])])
                self.assertEqual(self.actions(events, 'ipconfig'),
                                 [(3, ['set', 'en7', 'DHCP'])])

    def test_gateway_reset_restarts_no_lease_counter(self):
        events = self.run_watcher([{'ping': False}] * 4 + [{}] * 4)
        self.assertEqual(self.actions(events, 'ue200reset'), [(3, [])])
        self.assertEqual(self.actions(events, 'ipconfig')[-1],
                         (7, ['set', 'en7', 'DHCP']))

    def test_wifi_gateway_failure_does_not_reset_ue200_or_wifi(self):
        events = self.run_watcher([{'address': '192.0.2.20', 'route_interface': 'en0', 'ping': False}] * 8)
        self.assertEqual(self.actions(events, 'ue200reset'), [])
        self.assertEqual(self.actions(events, 'ipconfig'), [])

    def test_absent_or_unplugged_dongle_is_not_reset_for_gateway_loss(self):
        for state in ({'present': False}, {'active': False}):
            with self.subTest(state=state):
                events = self.run_watcher([dict(state, ping=False)] * 4)
                self.assertEqual(self.actions(events, 'ue200reset'), [])
                self.assertEqual(self.actions(events, 'ipconfig'), [])

    def test_gateway_reset_renews_rediscovered_device(self):
        events = self.run_watcher([{'address': '192.0.2.20', 'ping': False, 'after_reset': 'en9'}] * 4)
        self.assertEqual(self.actions(events, 'ipconfig'), [(3, ['set', 'en9', 'DHCP'])])


if __name__ == '__main__':
    unittest.main()
