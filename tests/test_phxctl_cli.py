"""Execute the real transport CLIs against a Linux-only fake IOKit."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

IOKIT = r'''
#ifndef FAKE_IOKIT_H
#define FAKE_IOKIT_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef int kern_return_t;
typedef unsigned io_connect_t;
typedef unsigned io_service_t;
#define kIOMainPortDefault 0
static inline unsigned mach_task_self(void) { return 1; }
static inline void *IOServiceMatching(const char *name) { (void)name; return (void *)1; }
static inline io_service_t IOServiceGetMatchingService(unsigned port, void *match) {
    (void)port; (void)match; return getenv("NO_SERVICE") ? 0 : 1;
}
static inline kern_return_t IOServiceOpen(io_service_t svc, unsigned task, unsigned type, io_connect_t *c) {
    (void)svc; (void)task; (void)type; *c = 1; return getenv("OPEN_FAIL") ? 0x123 : 0;
}
static inline int IOObjectRelease(unsigned obj) { (void)obj; return 0; }
static inline int IOServiceClose(unsigned obj) { (void)obj; return 0; }
static unsigned fake_seq;
static inline int event(unsigned sel, const uint64_t *in, unsigned ni, size_t bytes) {
    const char *path = getenv("IO_EVENTS");
    if (path) {
        FILE *f = fopen(path, "a");
        if (!f) { perror("events"); exit(99); }
        fprintf(f, "%u %zu", sel, bytes);
        for (unsigned i = 0; i < ni; i++) fprintf(f, " %llu", (unsigned long long)in[i]);
        fputc('\n', f); fclose(f);
    }
    static unsigned matches;
    const char *fail = getenv("FAIL_SEL");
    if (fail && sel == strtoul(fail, NULL, 0)) {
        matches++;
        const char *nth = getenv("FAIL_NTH");
        if (matches == (nth ? strtoul(nth, NULL, 0) : 1)) return 0x123;
    }
    return 0;
}
static inline kern_return_t IOConnectCallScalarMethod(io_connect_t c, unsigned sel,
    const uint64_t *in, unsigned ni, uint64_t *out, uint32_t *no) {
    (void)c;
    int kr = event(sel, in, ni, 0);
    if (kr) return kr;
    if (out && no) {
        memset(out, 0, *no * sizeof(*out));
        if (*no && sel == 100) out[0] = 1;
        const char *status = getenv("FW_STATUS");
        if (*no && status && (sel == 100 || sel == 103 || sel == 105 || sel == 106 || sel == 107))
            out[0] = strtoull(status, NULL, 0);
    }
    if (sel == 110 && in[0] == 0x16 * 4) fake_seq++;
    return 0;
}
static inline kern_return_t IOConnectCallMethod(io_connect_t c, unsigned sel,
    const uint64_t *in, unsigned ni, const void *data, size_t bytes,
    uint64_t *out, uint32_t *no, void *buf, size_t *sz) {
    (void)c; (void)data; (void)out; (void)no;
    int kr = event(sel, in, ni, bytes);
    if (kr) return kr;
    if (buf && sz) {
        memset(buf, 0, *sz);
        if (sel == 2 && *sz >= 4 && (in[0]==0xC040 || in[0]==0xC041)) *(uint32_t *)buf=0xDEADBEEF;
        if (sel == 102 && *sz >= 4) {
            if (in[0] == 0x2511000 + 32 && !getenv("MES_TIMEOUT")) *(uint32_t *)buf = fake_seq;
            if (in[0] == 0x2511000 + 64) *(uint32_t *)buf = 1;
            if (in[0] == 0x2520000 && !getenv("COMPUTE_TIMEOUT")) *(uint32_t *)buf = 0xFEEDF00D;
            if (in[0] == 0x2520040 && !getenv("GFX_TIMEOUT")) *(uint32_t *)buf = 0x6F7A6F7A;
        }
    }
    return 0;
}
static inline int fake_usleep(unsigned us) { (void)us; return 0; }
#define usleep fake_usleep
#endif
'''


class TransportCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.base = Path(cls.temp.name)
        headers = cls.base / 'IOKit'
        headers.mkdir()
        (headers / 'IOKitLib.h').write_text(IOKIT)
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise RuntimeError('C compiler required for CLI regression tests')
        for name in ('phxctl', 'mesboot'):
            subprocess.run([compiler, '-std=gnu11', '-O2', '-Wall', '-Wextra',
                            '-I', str(cls.base), str(ROOT / 'tools/phxctl' / (name + '.c')),
                            '-o', str(cls.base / name)], check=True, capture_output=True, text=True)
        cls.firmware = cls.base / 'fw.bin'
        cls.firmware.write_bytes(bytes(range(256)) * 32)

    def run_cli(self, name, args, **env):
        events = self.base / 'events'
        events.write_text('')
        result = subprocess.run([str(self.base / name), *map(str, args)],
                                env=dict(os.environ, IO_EVENTS=str(events), **env),
                                capture_output=True, text=True, timeout=10)
        calls = [list(map(int, line.split())) for line in events.read_text().splitlines()]
        return result, calls

    def phx_commands(self):
        return [(['regs', '1', '2'], 2), (['gc'], 2), (['smu', '1'], 100),
                (['smuinfo'], 100), (['psp', 'status'], 101), (['psp', 'create', '0'], 101),
                (['psp', 'destroy'], 101), (['vram', '0'], 102), (['submit', '1'], 103),
                (['tmr'], 106), (['autoload'], 106),
                (['loadfw', self.firmware, '0', '5000', '1', '0'], 105),
                (['stage', self.firmware, '0', '5000', '0'], 104),
                (['wreg', '1', '2'], 108), (['vramw', '0', '1'], 109),
                (['pte', '0', '2', '4096', '0x71'], 109), (['raw', '1'], 107)]

    def test_phxctl_transport_failure_and_success(self):
        for args, selector in self.phx_commands():
            for fail in (False, True):
                with self.subTest(args=args, fail=fail):
                    result, calls = self.run_cli('phxctl', args, **({'FAIL_SEL': str(selector)} if fail else {}))
                    self.assertEqual(result.returncode, int(fail), result.stdout + result.stderr)
                    self.assertIn(selector, [call[0] for call in calls])
                    self.assertTrue(result.stdout)

    def test_firmware_status_is_failure_even_if_transport_succeeds(self):
        for args, selector in self.phx_commands():
            if selector not in (100, 103, 105, 106, 107):
                continue
            for status in ('0xff', '0xffffffff'):
                with self.subTest(args=args, status=status):
                    result, _ = self.run_cli('phxctl', args, FW_STATUS=status)
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_failed_aggregate_read_is_not_cleared_by_later_success(self):
        for args, selector, count in ((['regs', '1', '2'], 2, 2), (['smuinfo'], 100, 5)):
            result, calls = self.run_cli('phxctl', args, FAIL_SEL=str(selector))
            self.assertEqual(result.returncode, 1)
            self.assertEqual(sum(call[0] == selector for call in calls), count)

    def test_truncated_firmware_is_never_submitted(self):
        for command in ('loadfw', 'stage'):
            for offset, size in ((0, 9000), (8192, 1), (9000, 1), (0, 8193)):
                args = [command, self.firmware, offset, size]
                args += [1, 0] if command == 'loadfw' else [0]
                with self.subTest(command=command, offset=offset, size=size):
                    result, calls = self.run_cli('phxctl', args)
                    self.assertEqual(result.returncode, 1)
                    self.assertIn('short firmware read', result.stderr)
                    self.assertNotIn(105, [call[0] for call in calls])

    def test_stage_failure_aborts_firmware_load(self):
        for nth in ('1', '2'):
            result, calls = self.run_cli('phxctl', ['loadfw', self.firmware, 0, 5000, 1, 0],
                                         FAIL_SEL='104', FAIL_NTH=nth)
            self.assertEqual(result.returncode, 1)
            self.assertIn('stage @', result.stdout)
            self.assertNotIn(105, [call[0] for call in calls])

    def test_firmware_read_error_is_not_submitted(self):
        for command in ('loadfw', 'stage'):
            args = [command, self.base, 0, 1]
            args += [1, 0] if command == 'loadfw' else [0]
            result, calls = self.run_cli('phxctl', args)
            self.assertEqual(result.returncode, 1)
            self.assertIn('read:', result.stderr)
            self.assertNotIn(105, [call[0] for call in calls])

    def test_pte_high_word_failure_stops_writing(self):
        result, calls = self.run_cli('phxctl', ['pte', 0, 2, 4096, '0x71'],
                                     FAIL_SEL='109', FAIL_NTH='2')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(sum(call[0] == 109 for call in calls), 2)

    def test_firmware_range_and_chunk_sizes_are_preserved(self):
        result, calls = self.run_cli('phxctl', ['loadfw', self.firmware, 123, 5000, 7, 64])
        self.assertEqual(result.returncode, 0)
        self.assertEqual([call for call in calls if call[0] == 104],
                         [[104, 4096, 64], [104, 904, 4160]])
        self.assertEqual([call for call in calls if call[0] == 105], [[105, 0, 7, 64, 5000]])

    def test_mesboot_shared_helpers_claim_and_adopt(self):
        cases = [(['mes'], {}, 108), (['dump'], {}, 2), (['db', 0, 1], {}, 110),
                 (['kiq'], {}, 109), (['gfx'], {}, 102), (['sdmasys'], {}, 111),
                 (['gfx'], {'ADOPT_GFX': '1'}, 112),
                 (['sched'], {'MESBOOT_CS': '1', 'ADOPT': '1'}, 112), (['claim'], {}, 113)]
        for args, env, selector in cases:
            for fail in (False, True):
                with self.subTest(args=args, selector=selector, fail=fail):
                    result, calls = self.run_cli('mesboot', args, **env,
                                                 **({'FAIL_SEL': str(selector)} if fail else {}))
                    self.assertEqual(result.returncode, int(fail), result.stdout + result.stderr)
                    self.assertIn(selector, [call[0] for call in calls])
                    if fail:
                        self.assertIn('0x123', result.stdout + result.stderr)

    def test_mesboot_stops_after_transport_failure(self):
        result, calls = self.run_cli('mesboot', ['mes'], FAIL_SEL='108')
        self.assertEqual(result.returncode, 1)
        self.assertEqual([call[0] for call in calls], [0, 108])

    def test_unfinished_queues_are_not_adopted(self):
        cases = [(['sched'], {'MESBOOT_CS':'1', 'ADOPT':'1', 'MES_TIMEOUT':'1'}),
                 (['sched'], {'MESBOOT_CS':'1', 'ADOPT':'1', 'COMPUTE_TIMEOUT':'1'}),
                 (['gfx'], {'ADOPT_GFX':'1', 'GFX_TIMEOUT':'1'})]
        for args, env in cases:
            with self.subTest(args=args,env=env):
                result, calls = self.run_cli('mesboot', args, **env)
                self.assertEqual(result.returncode, 1)
                self.assertNotIn(112, [call[0] for call in calls])

    def test_connection_failures(self):
        for name, args in (('phxctl', ['gc']), ('mesboot', ['claim'])):
            for env in ({'NO_SERVICE': '1'}, {'OPEN_FAIL': '1'}, {'FAIL_SEL': '0'}):
                with self.subTest(name=name, env=env):
                    result, _ = self.run_cli(name, args, **env)
                    self.assertEqual(result.returncode, 1)
                    self.assertTrue(result.stderr)


if __name__ == '__main__':
    unittest.main()
