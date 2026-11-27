"""Run bring-up scripts with fake GPU commands, watchdog and filesystem paths."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FAKE = r'''
import json,os,sys
from pathlib import Path
base=Path(os.environ['BOOT_TEST'])
name=Path(sys.argv[0]).name
args=sys.argv[1:]
with (base/'events').open('a') as f: f.write(json.dumps([name,args])+'\n')
command=' '.join([name]+args[:2])
if name=='id': print('0')
elif name=='ioreg':
    for key, value in json.loads(os.environ.get('BOOT_FB_LAYOUT','{}')).items():
        print('    \"'+key+'\" = '+str(value))
    if os.environ.get('BOOT_M2')=='1':
        print('class '+ ('PhxAMDFB' if 'PhxAMDFB' in args else 'NMXAccel' if 'NMXAccel' in args else 'IOFramebuffer'))
    else: print('class IONDRVFramebuffer')
    print('\"phx-build\" = 34')
elif name=='phxctl': print('loadfw result' if args[0]=='loadfw' else 'ok')
elif name=='mesboot': print('compute: ready')
if command==os.environ.get('BOOT_FAIL') or (name=='phxctl' and args[0]=='loadfw' and os.environ.get('BOOT_FAIL')=='loadfw'): sys.exit(7)
if name=='kill': sys.exit(int(os.environ.get('KILL_STATUS','0')))
'''


class BringupFailureTest(unittest.TestCase):
    def run_script(self, name, fail='', kill_status=0, boot_full_status=0, m2=False, arming=False, layout=None):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            gpu = base / 'gpu'
            (gpu / 'phxctl').mkdir(parents=True)
            commands = base / 'bin'
            commands.mkdir()
            fake = commands / 'fake'
            fake.write_text('#!' + sys.executable + '\n' + FAKE)
            fake.chmod(0o755)
            for command in ('id','sleep','shutdown','kill','sysctl','sync','nvram','ioreg'):
                (commands / command).symlink_to(fake)
            for command in ('phxctl','mesboot'):
                (gpu / 'phxctl' / command).symlink_to(fake)
            for script in ('bringup5.sh','full.sh'):
                text = (ROOT / 'tools/phxctl' / script).read_text().replace('~/gpu',str(gpu)).replace('/bin/kill',str(commands/'kill'))
                (gpu / script).write_text(text)
            path = gpu / name
            if name == 'phxboot.sh':
                text = (ROOT / 'tools/phxctl/phxboot.sh').read_text()
                text = text.replace('G=/Users/Shared/gpu; export HOME=/Users/Shared','G='+str(gpu))
                text = text.replace('/var/log/phxboot.log',str(base/'boot.log'))
                text = text.replace('/var/db/phxboot.pending',str(base/'pending'))
                text = text.replace('/Library/GPUBundles/nvmtl-allow.txt',str(base/'allow'))
                text = text.replace('/Library/Logs/DiagnosticReports',str(base/'reports'))
                # Exercise the real dependency checks and stop before normal configuration/watcher.
                end = text.index('# a copied frame completes' if arming else '# no BO cache flushing per submit/wait')
                path.write_text(text[:end]+('fi\n' if arming else '')+'echo reached >> "$G/../reached"\n')
                (gpu/'full.sh').write_text('echo "compute: attempted"\nexit '+str(boot_full_status)+'\n')
            result = subprocess.run(['bash', str(path),'kiqsched'],capture_output=True,text=True,timeout=10,
                                    env=dict(os.environ,PATH=str(commands)+':/usr/bin:/bin',BOOT_TEST=str(base),
                                             BOOT_FAIL=fail,KILL_STATUS=str(kill_status),BOOT_M2=str(int(m2)),
                                             BOOT_FB_LAYOUT=json.dumps(layout if layout is not None else {
                                                 'phx-fb-phys': 0x6A04000000, 'phx-fb-mc-base': 0x8000000000,
                                                 'phx-fb-size': 0x20000000, 'phx-fb-aper-len': 0x10000000})))
            events = [json.loads(s) for s in (base/'events').read_text().splitlines()]
            return result.returncode, events, (base/'reached').exists()

    def test_tmr_uses_captured_physical_and_mc_addresses(self):
        for physical, mc in ((0x6A04000000, 0x8000000000), (0x17E0000000, 0x9000000000)):
            with self.subTest(physical=physical):
                rc, events, _ = self.run_script('bringup5.sh', layout={
                    'phx-fb-phys': physical, 'phx-fb-mc-base': mc,
                    'phx-fb-size': 0x80000000, 'phx-fb-aper-len': 0x10000000})
                self.assertEqual(rc, 0)
                commands = [[int(a, 0) for a in args[1:]] for name,args in events
                            if name == 'phxctl' and args[0] == 'raw']
                self.assertIn([0x20, (mc + 0x4000000) & 0xffffffff,
                               (mc + 0x4000000) >> 32, 0x900, 0, 0, 0], commands)
                self.assertIn([5, (mc + 0x8000000) & 0xffffffff,
                               (mc + 0x8000000) >> 32, 0x4000000, 2,
                               (physical + 0x8000000) & 0xffffffff,
                               (physical + 0x8000000) >> 32], commands)

    def test_invalid_layout_aborts_before_hardware_or_watchdog(self):
        valid = {'phx-fb-phys': 0x17E0000000, 'phx-fb-mc-base': 0x8000000000,
                 'phx-fb-size': 0x80000000, 'phx-fb-aper-len': 0x10000000}
        invalid = [{}]
        for key in valid:
            for value in ('missing', 'bad', -1, 0, 0x7fffffffffffffff):
                layout = dict(valid)
                if value == 'missing': del layout[key]
                else: layout[key] = value
                invalid.append(layout)
        invalid += [dict(valid, **{key: value}) for key,value in (
            ('phx-fb-phys', 0x17E0000001), ('phx-fb-mc-base', 0x8000000001),
            ('phx-fb-size', 0xbffffff), ('phx-fb-aper-len', 0xbffffff))]
        for layout in invalid:
            with self.subTest(layout=layout):
                rc, events, _ = self.run_script('bringup5.sh', layout=layout)
                self.assertNotEqual(rc, 0)
                self.assertFalse(any(name in ('phxctl', 'shutdown', 'kill') for name,args in events))

    def test_create_failure_aborts_before_firmware(self):
        rc, events, _ = self.run_script('bringup5.sh', 'phxctl psp create')
        self.assertEqual(rc,7)
        self.assertFalse(any(e[0]=='phxctl' and e[1][0]=='stage' for e in events))
        self.assertFalse(any(e[0]=='kill' for e in events))

    def test_firmware_and_smu_failures_survive_output_pipelines(self):
        for fail in ('phxctl loadfw', 'phxctl smu 0x3'):
            with self.subTest(fail=fail):
                target = 'loadfw' if fail=='phxctl loadfw' else fail
                rc, events, _ = self.run_script('bringup5.sh', target)
                self.assertEqual(rc,7)
                self.assertFalse(any(e[0]=='kill' for e in events))

    def test_best_effort_destroy_does_not_abort(self):
        rc, _, _ = self.run_script('bringup5.sh','phxctl psp destroy')
        self.assertEqual(rc,0)

    def test_missing_watchdog_process_is_benign_after_success(self):
        rc, _, _ = self.run_script('full.sh',kill_status=1)
        self.assertEqual(rc,0)

    def test_full_does_not_continue_after_failed_powerup(self):
        rc, events, _ = self.run_script('full.sh','phxctl psp create')
        self.assertEqual(rc,7)
        self.assertFalse(any(e[0]=='mesboot' for e in events))

    def test_full_keeps_watchdog_after_failed_mes_step(self):
        for step in ('gart','mes','kiqsched'):
            with self.subTest(step=step):
                rc, events, _ = self.run_script('full.sh','mesboot '+step)
                self.assertEqual(rc,7)
                self.assertEqual(sum(e[0]=='kill' for e in events),1)

    def test_boot_rejects_full_failure_despite_matching_output(self):
        rc, events, reached = self.run_script('phxboot.sh',boot_full_status=7)
        self.assertEqual(rc,1)
        self.assertFalse(reached)
        self.assertFalse(any(e[0]=='mesboot' for e in events))

    def test_boot_rejects_claim_failure(self):
        rc, _, reached = self.run_script('phxboot.sh','mesboot claim')
        self.assertEqual(rc,1)
        self.assertFalse(reached)

    def test_success_reaches_configuration(self):
        rc, _, reached = self.run_script('phxboot.sh')
        self.assertEqual(rc,0)
        self.assertTrue(reached)

    def test_each_successful_phase_cancels_only_its_own_pid(self):
        rc, events, _ = self.run_script('full.sh')
        self.assertEqual(rc,0)
        targets = [args for name,args in events if name=='kill']
        self.assertEqual(len(targets),2)
        self.assertTrue(all(len(args)==1 and args[0].isdigit() and int(args[0])>0 for args in targets))
        self.assertNotEqual(targets[0],targets[1])

    def test_m2_failure_reboots_without_claiming_or_rearming(self):
        rc, events, reached = self.run_script('phxboot.sh',boot_full_status=7,m2=True)
        self.assertEqual(rc,0)  # m2_fail requests safe fallback reboot and exits.
        self.assertFalse(reached)
        self.assertIn(['shutdown',['-r','now']],events)
        self.assertFalse(any(name=='mesboot' for name,args in events))
        self.assertFalse(any(name=='nvram' and 'phxfb-boot=1' in args for name,args in events))

    def test_arming_failure_reboots_before_release(self):
        for setting in ('nvaccelfb=1','nvaccel_iop=1','nvaccelfb=3','phxagdc=1'):
            with self.subTest(setting=setting):
                rc, events, reached = self.run_script('phxboot.sh','sysctl -w debug.'+setting,m2=True,arming=True)
                self.assertEqual(rc,0)
                self.assertFalse(reached)
                self.assertIn(['shutdown',['-r','now']],events)

if __name__ == '__main__':
    unittest.main()
