# Framebuffer-fetch descriptor mask regression test

Run `make test-nmplugin-descriptor-mask` with Clang, Python 3 and Vulkan headers.
On macOS with headers outside the SDK, set `CPPFLAGS=-I/path/to/Vulkan-Headers/include`.
The runner extracts the actual production parser, mask initializer expressions,
descriptor snapshot/reuse functions and writer, and compiles them with ASan/UBSan.
Only Vulkan allocation/update endpoints and runtime controls are CPU test doubles.

Tests run in separate processes with `NVMTL_DESC_MASK` unset, `0`, and `1`.
They check framebuffer-fetch set exclusion, normal buffer/image/sampler/address
bindings, storage images, texel-buffer changes, descriptor reuse/copies,
legacy/all-bindings fallbacks, compute initialization, tessellation and H3-off
fallbacks, allocation/address failure, and invalid SPIR-V instruction lengths.
Optional SPIR-V files may be passed to `python3 tests/test_nmplugin_descriptor_mask.py`.
Generated files stay under `build/tests/descriptor_mask`.

The legacy wrapper is checked against the explicit legacy parser mode. This is
not an independent oracle or a shader semantic validator. The synthetic writer
checks verify emitted binding identities and counts, not GPU execution or FPS.
