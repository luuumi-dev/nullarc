#!/usr/bin/env python3
"""Exercise production mask parsing, pipeline initialization and descriptor writing.

Vulkan allocation/update endpoints are CPU doubles; no GPU or driver is needed.
Pass optional SPIR-V fixture paths for additional parser compatibility checks.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/tests/descriptor_mask"


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    text = (ROOT / "src/nmplugin/nvmtl_vk.c").read_text()
    pieces = [
        """/* Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef __APPLE__
#include <mach/mach.h>
#else
typedef unsigned int mach_port_t;
#endif
#include <vulkan/vulkan.h>
#include "nvmtl_vk.h"
static void nvlog(const char *format, ...) { (void)format; }
""",
        text[text.index("#define NVMTL_NBUF "):text.index("#define NVMTL_MAX_SETS")],
        function(text, "static int nvmtl_desc_mask_on(void)"),
        "static int nvmtl_spirv_bmask_x(const void *, size_t, uint64_t [4], int);",
        function(text, "static int nvmtl_spirv_bmask_x(const void *p, size_t n, uint64_t out[4], int skip_fetch)\n{"),
        function(text, "static int nvmtl_spirv_bmask(const void *p, size_t n, uint64_t out[4])\n{"),
        text[text.index("enum { NVMTL_SLOT_NONE"):text.index("static VkDescriptorSetLayout g_setLayout;")],
        '#include "cpu_stubs.h"',
        function(text, "static VkDescriptorType binding_type(uint32_t i)"),
        text[text.index("#define NVMTL_DESC_BATCH_MAX"):text.index("int nvmtl_vk_bind_buffer(")],
    ]
    # Read the actual graphics/compute initialization expressions, rather than
    # duplicating their behavior in a test implementation.
    def line(prefix):
        matches = [s.strip() for s in text.splitlines() if s.strip().startswith(prefix)]
        assert len(matches) == 1, prefix
        return matches[0]
    pieces += [
        "static void graphics_masks(const void *vs, size_t vn, const void *fs, size_t fn, const void *td, nvk_pipeline *out) { "
        + line("if (!td) out->gmask_ok =") + line("if (!td) out->gmask2_ok =") + " }",
        "static void compute_masks(const void *cs, size_t n, nvk_pipeline *out) { "
        + line("out->bmask_ok =") + line("out->bmask2_ok =") + " }",
        '#include "cpu_cases.h"',
    ]
    BUILD.mkdir(parents=True, exist_ok=True)
    generated = BUILD / "production.c"
    generated.write_text("\n".join(pieces) + "\n")
    binary = BUILD / "test"
    command = shlex.split(os.environ.get("CC", "clang")) + [
        "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
        "-I" + str(ROOT / "src/nmplugin"), "-I" + str(ROOT / "tests/descriptor_mask"),
    ] + shlex.split(os.environ.get("CPPFLAGS", "")) + [str(generated), "-pthread", "-o", str(binary)]
    subprocess.run(command, check=True)
    for mode in [None, "0", "1"]:
        env = {k: v for k, v in os.environ.items() if not k.startswith("NVMTL_")}
        if mode is not None:
            env["NVMTL_DESC_MASK"] = mode
        print("NVMTL_DESC_MASK=" + (mode or "unset"), flush=True)
        subprocess.run([str(binary)] + sys.argv[1:], env=env, check=True, timeout=60)
        print()


if __name__ == "__main__":
    main()
