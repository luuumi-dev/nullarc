#!/usr/bin/env python3
"""CPU-only PR28 deadline, publication and reentrancy regressions.

Compile the actual helpers and completion method with Foundation/libdispatch;
only surfaces, command execution, callbacks and optional clock samples are
doubles. No Metal device, plugin loading, Vulkan call or GPU work is involved.
"""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/tests/surface_commit_wait"


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class SurfaceCommitWait(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        events = (ROOT / "src/nmplugin/NVMTLEventSync.m").read_text()
        objects = (ROOT / "src/nmplugin/NVMTLObjects.m").read_text()
        cls.events, cls.objects = events, objects
        source = (ROOT / "tests/surface_commit_wait/fixtures.m").read_text()
        source += "\nstatic char nvmtl_execution_queue_key;\n"
        source += function(objects, "static BOOL nvmtl_cb_writes_surfaces(") + "\n"
        for signature in [
            "static BOOL nvmtl_surface_commit_wait_on(",
            "static unsigned nvmtl_surface_commit_wait_ms(",
            "static void nvmtl_surface_commit_prepare(",
            "static BOOL nvmtl_surface_commit_wait(",
        ]:
            source += function(events, signature) + "\n"
        source += "\n#undef dispatch_group_wait\n#undef clock_gettime_nsec_np\n"
        source += "@implementation NVMTLCommandBuffer\n"
        source += "- (void)nvmtlDispatchHandlers:(NSArray *)h scheduled:(BOOL)s { queuedHandlers = h; }\n"
        source += function(events, "- (void)nvmtlFinishCallbacks {") + "\n@end\n"
        source += (ROOT / "tests/surface_commit_wait/cases.m").read_text()
        BUILD.mkdir(parents=True, exist_ok=True)
        path = BUILD / "production.m"
        path.write_text(source)
        cls.binary = BUILD / "test"
        subprocess.run([
            "xcrun", "clang", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-fobjc-arc", "-fblocks",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer", str(path), "-framework", "Foundation",
            "-o", str(cls.binary),
        ], check=True, timeout=60)

    def run_case(self, case, enabled="1", budget="8", program="CameraApp", kill=False, expected=None):
        env = {k: v for k, v in os.environ.items() if not k.startswith("NVMTL_")}
        env["ASAN_OPTIONS"] = "detect_leaks=0"
        if enabled is not None:
            env["NVMTL_SURFACE_COMMIT_WAIT"] = enabled
        if budget is not None:
            env["NVMTL_SURFACE_COMMIT_WAIT_MS"] = budget
        if kill:
            env["NVMTL_NO_SURFACE_COMMIT_WAIT"] = "1"
        args = [str(self.binary), case, program]
        if expected is not None:
            args.append(str(expected))
        subprocess.run(args, env=env, check=True, timeout=10)

    def test_policy(self):
        for enabled, program, kill, expected in [
            (None, "CameraApp", False, 0), ("0", "CameraApp", False, 0),
            ("true", "CameraApp", False, 0), ("1x", "CameraApp", False, 0),
            ("1", "CameraApp", False, 1), ("1", "WindowServer", False, 0),
            ("1", "CameraApp", True, 0),
        ]:
            with self.subTest(enabled=enabled, program=program, kill=kill):
                self.run_case("policy", enabled=enabled, program=program, kill=kill, expected=expected)

    def test_budget(self):
        for value, expected in [(None, 250), ("1", 1), ("2000", 2000), ("0", 250),
                                ("2001", 250), ("-1", 250), ("+1", 250), ("", 250),
                                ("1x", 250), (" 1", 250), ("184467440737095516160", 250)]:
            with self.subTest(value=value):
                self.run_case("budget", budget=value, expected=expected)

    def test_no_surface_or_group(self):
        self.run_case("no-surface")

    def test_completion_and_copy_precede_return_without_handlers(self):
        self.run_case("completion", budget="2000")

    def test_timeout_retains_pending_state_then_late_completion(self):
        self.run_case("timeout")

    def test_elapsed_samples_not_requested_delay(self):
        self.run_case("clock")

    def test_execution_identity_target_inheritance_and_label_collision(self):
        self.run_case("worker")

    def test_all_waiters_wake(self):
        self.run_case("many", budget="2000")

    def test_error_completion_is_not_relabelled_success(self):
        self.run_case("error")

    def test_production_wiring(self):
        submit = function(self.events, "- (void)nvmtlSubmitWithEvents {")
        self.assertLess(submit.index("nvmtlSealPartWithSignal:"), submit.index("nvmtl_surface_commit_prepare(self)"))
        self.assertLess(submit.index("nvmtl_surface_commit_prepare(self)"), submit.index("nvmtl_pump_buffers()"))
        commit = function(self.objects, "- (void)commit {")
        self.assertIn("if (!each) (void)nvmtl_surface_commit_wait(self);", commit)
        self.assertNotIn("nvmtl_cb_writes_surfaces", commit)
        wait = function(self.events, "static BOOL nvmtl_surface_commit_wait(")
        self.assertIn("dispatch_time(DISPATCH_TIME_NOW,", wait)
        self.assertNotIn("DISPATCH_TIME_FOREVER", wait)
        self.assertNotIn("NSDate", wait)
        self.assertNotIn("_callbackGroup", wait)
        self.assertIn("dispatch_queue_set_specific(worker, &nvmtl_execution_queue_key", self.events)
        finish = function(self.events, "- (void)nvmtlFinishCallbacks {")
        self.assertLess(finish.index("_completionReady = YES"), finish.index("dispatch_group_leave(_surfaceCommitGroup)"))
        self.assertLess(finish.index("[_completionCondition unlock]"), finish.index("dispatch_group_leave(_surfaceCommitGroup)"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
