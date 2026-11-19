#!/usr/bin/env python3
"""Execute actual PhxAMDFB alias/pool methods under ASan/UBSan with CPU doubles."""
import hashlib
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def method(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    source = (ROOT / "src/phxaccel/accel/phxfb.cpp").read_text()
    template = (ROOT / "tests/upstream34_framebuffer_cases.cpp").read_text()
    for marker, signature in {
        "// ACTUAL_ALIAS_METHOD": "IOMemoryMap *aliasMap()",
        "// ACTUAL_POOL_INIT": "void poolInit()",
        "// ACTUAL_POOL_FREE": "IOReturn vramFree(NVRMVramRequest *r)",
    }.items():
        template = template.replace(marker, method(source, signature))
    build = ROOT / "build/tests/upstream34-framebuffer"
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
    print("hardware_executed=false; IOKit mappings/allocations are CPU doubles")


if __name__ == "__main__":
    main()
