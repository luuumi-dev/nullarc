#!/usr/bin/env python3
"""Exercise actual A3 callbacks with CPU mapping/clock doubles and sanitizers."""
import hashlib
from pathlib import Path
import subprocess
from test_native_bo_lifetime import method

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / "src/phxaccel/accel/phxfb.cpp").read_text()
    template = (ROOT / "tests/upstream34_panel_callbacks.cpp").read_text()
    for marker, signature in {
        "// ACTUAL_REFRESH_METHOD": "void measureRefresh()",
        "// ACTUAL_TIMING_METHOD": "IOReturn getTimingInfoForDisplayMode(",
    }.items():
        template = template.replace(marker, method(source, signature).replace(" override", ""))
    build = ROOT / "build/tests/upstream34-panel-callbacks"
    build.mkdir(parents=True, exist_ok=True)
    generated = build / "production.cpp"
    generated.write_text(template)
    binary = build / "test"
    subprocess.run(["clang++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer", "-I" + str(ROOT / "src/phxaccel"),
                    str(generated), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
    print("extracted_production_harness_sha256=" + hashlib.sha256(generated.read_bytes()).hexdigest())
    print("hardware_executed=false; MMIO, mappings and clocks are CPU doubles")


if __name__ == "__main__":
    main()
