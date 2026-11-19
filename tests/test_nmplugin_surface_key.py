#!/usr/bin/env python3
"""Compile the production registry/monitor functions with CPU-only IOSurface doubles."""
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/tests/surface_key"


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    events = (ROOT / "src/nmplugin/NVMTLEventSync.m").read_text()
    objects = (ROOT / "src/nmplugin/NVMTLObjects.m").read_text()
    definitions = (ROOT / "tests/surface_key_fixtures.m").read_text()
    cases = (ROOT / "tests/surface_key_cases.m").read_text()
    signatures = [
        "static id nvmtl_surface_writer_key(",
        "static BOOL nvmtl_surface_pending_write(",
        "static void nvmtl_surface_register_writes(",
        "static void nvmtl_surface_finish_writes(",
    ]
    source = definitions + "\n" + "\n".join(function(events, s) for s in signatures)
    source += "\n" + function(objects, "static id nvmtl_surface_sync(") + "\n" + cases
    BUILD.mkdir(parents=True, exist_ok=True)
    generated = BUILD / "production.m"
    generated.write_text(source)
    binary = BUILD / "test"
    command = shlex.split(os.environ.get("OBJC", "clang")) + [
        "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fobjc-arc", "-fblocks",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-fno-omit-frame-pointer", "-I" + str(ROOT / "src/nmplugin"),
        str(generated), "-framework", "Foundation", "-o", str(binary),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
