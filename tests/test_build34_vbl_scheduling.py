#!/usr/bin/env python3
"""Run actual VBL callback/allocation/drain with CPU clock and scheduler doubles."""
import hashlib
from pathlib import Path
import plistlib
import subprocess

from source_methods import method

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / "src/phxaccel/accel/phxfb.cpp").read_text()
    with (ROOT / "src/phxaccel/Info.plist").open("rb") as stream:
        libraries = plistlib.load(stream)["OSBundleLibraries"]
    # A target build can return zero but omit a third-party kext using the
    # Apple-only private KPI. Public IOKit and exact cache readback are required.
    assert "com.apple.kpi.private" not in libraries, "private KPI is Apple-only"
    assert "thread_call_cancel_wait(" not in source, "cancel/wait has no public KPI export"
    start = method(source, "bool PhxAMDFB::start(IOService *provider)")
    allocation = start[start.index("        fVblWorkLoop = IOWorkLoop::workLoop();"):
                       start.index("        static IOPMPowerState states[2]")]
    stop = method(source, "void PhxAMDFB::stop(IOService *provider)")
    drain = stop[stop.index("    __atomic_store_n(&fStopping"):
                 stop.index("    if (gPhxPrimaryFB")]
    drain += "    phxAgdcRemove();\n"
    template = (ROOT / "tests/build34_vbl_scheduling.cpp").read_text()
    for marker, value in {
        "// ACTUAL_VBL_CALLBACK": method(source, "static void vblFire("),
        "// ACTUAL_VBL_RETIRE": method(source, "static IOReturn vblRetire("),
        "// ACTUAL_VBL_ALLOCATION": allocation,
        "// ACTUAL_VBL_DRAIN": drain,
    }.items():
        template = template.replace(marker, value)
    build = ROOT / "build/tests/build34-vbl-scheduling"
    build.mkdir(parents=True, exist_ok=True)
    generated = build / "production.cpp"
    generated.write_text(template)
    binary = build / "test"
    subprocess.run([
        "clang++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-fno-omit-frame-pointer", str(generated), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
    print("extracted_production_harness_sha256=" + hashlib.sha256(generated.read_bytes()).hexdigest())
    print("hardware_executed=false; XNU scheduling/clock behavior is modeled, not measured")
    print("Apple-only_KPI=absent; target AuxKC candidate inclusion remains required")


if __name__ == "__main__":
    main()
