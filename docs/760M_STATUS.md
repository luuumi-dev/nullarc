# Radeon 760M development status

Updated 2026-11-08. Fork: `yt42c2mvn2-cmd/nullarc-760m`.
Branch: `codex/radeon-760m-integration`. Baseline: `85e4142`.

**HP acceleration is not functional or hardware-tested. This branch intentionally
allows only diagnostics on the captured HP PCI identity. Do not install an upstream
780M release expecting these source changes.** No system drivers were installed,
loaded, or replaced during this session.

## Stages and changed components

| Stage | Result | Files/components changed | Validation |
|---|---|---|---|
| 1: inspect | Completed architecture audit; old R3N path is a simulation | `docs/760M_ARCHITECTURE.md` | README and linked notes cross-checked against active sources |
| 2: profile | Exact HP identity; physical masks retained; strict JSON/bitmap validation | `hardware/profiles/hp-760m.json`, `src/amd/phx_{topology,hp_760m}.h`, `tools/phx_profile.py`, profile tests | Header reproduction; malformed/count/range/sparse-mask/4-CU/12-CU tests |
| 3: bring-up | Diagnostic-only attachment; selector 30 exports captured topology; no HP DMA enable, writes, BOs, submit, queue adoption or accelerator claim | `src/phxbringup/{PhxBringup.cpp,PhxNativeABI.h}`, `tools/phxctl/{bringup5,full,phxboot}.sh` | Native x86_64 kext build; ABI size assertions; selector policy tests; shell syntax; only new kernel import is standard IOPCIDevice byte-config read |
| 4: translation/backend | Runtime topology reaches RADV DEV_INFO, CU mapping, enabled RBs and PCI identity; old fallback retained | `mesa-patches-phoenix/0007-*`, `tools/mesa_phoenix_topology.py`, Mesa tests/runners | Complete x86_64 RADV build; mock Vulkan reports 8 CUs; gfx1103 ACO pipeline compiled |
| 5: accelerator | HP framebuffer and NMXAccel decline before takeover, independent of service start order | `src/phxaccel/accel/{phxfb,nvrm-accel}.cpp` | Native x86_64 PhxAccel build; no runtime loading |
| 6: integration | Read-only macOS profile inspector; Linux DRM/IP collector; host tests and Linux collector CI build | `tools/phxctl/phxprofile.c`, `tools/linux-profile/collect.c`, `Makefile`, `.github/workflows/ci.yml` | macOS inspector compiles with warnings as errors; Linux collector awaits Linux CI/hardware |
| 7: tests | Host and mock coverage passes; no hardware result | `tests/test_phx_*`, `tests/test_mesa_*`, `tools/test_mesa_*` | 5 host C++ test binaries, 4 Python test methods; firmware fixtures incl. 5000 mutations; Mesa marshaller 134 checks plus new topology tests; ASan/UBSan for topology and transport; byte-identical patch/generator roundtrip; 8-CU and unchanged 12-CU Vulkan mock + ACO |
| 8: docs | Usage, evidence requirements and source dependency map | README, install caveat, Mesa patch instructions, status/architecture docs | Commands checked where this Mac provides dependencies |

Native builds use the documented `src/phxbringup/build.sh` and
`src/phxaccel/build.sh` with MacKernelSDK. Three pre-existing field-shadow warnings
remain in PhxBringup. PhxAccel builds cleanly. Standard unresolved kernel imports
are expected in linked kexts; KXLD/AuxKC load compatibility is untested.

## Authoritative supplied capture

`hp-760m.json` was supplied by the user and copied unchanged. Its original collection
method/date were not supplied. Validation establishes internal consistency, not
independent measurement of this machine:

- PCI `1002:1900`, revision `ca`, subsystem `103c:8c6a`.
- 1 shader engine; 2 shader arrays; 6 physical CU slots per array.
- 8 active CUs: masks `[0x0f, 0x0f]`; do not derive masks from CU count.
- 4 physical RB pipes, enabled mask `0x0a`; preserve physical indexing.
- No IP-discovery versions, GB_ADDR_CONFIG, firmware hashes/addresses, memory
  aperture/reservation map, panel configuration, or validated initialization trace.

The private 128-byte topology query is separate from the existing 192-byte N48N
QueryInfo. Its `CAPTURED` flag never implies `RUNTIME_READY`; this branch never sets
that readiness bit for HP. Unavailable legacy query uses the original canned path;
malformed topology is rejected. A nonempty invalid mock-profile name is refused.

## Reproduce

```sh
make fixtures
REQUIRE_FIXTURES=1 make test
sh src/phxbringup/build.sh /path/to/MacKernelSDK
sh src/phxaccel/build.sh /path/to/MacKernelSDK
```

Apply Cobalt64 0001..0009, then this repository's Phoenix 0001..0007 to Mesa
`f5cb8ee032adabef599ae892ec4e42d796b84da9`. The generator is an alternative to
0007, not an additional patch. Build per `mesa-patches-phoenix/README.txt`, then:

```sh
sh tools/test_mesa_topology.sh /path/to/mesa
python3 tools/test_mesa_vulkan_profile.py /path/to/mesa --glslang /path/to/glslangValidator
```

Both tests are mocks. They verify topology transport/compiler integration, not GPU
execution, Metal, textures, scanout, interrupts or scheduler behavior on HP.
The AIR translator is an external prebuilt library: its source is not in this fork;
no AIR translation or full Metal bundle build was performed.

## Next required hardware evidence

On Linux booted on the HP, build/run `tools/linux-profile/collect.c` as documented
at its top. Validate the resulting JSON with `python3 tools/phx_profile.py captured.json`.
Keep stderr's IP query results. Also collect the target's PCI `resource` file,
full `device/ip_discovery/` sysfs tree if available, amdgpu initialization log and
matching firmware filenames/hashes. The generic DRM HW_IP query alone does not
identify PSP, SMU, MMHUB or DCN sufficiently.

Next, compare the HP IP/base-address table and firmware layout with the reference,
validate memory reservations against the live GOP scanout, then replace the Firebat
constants with checked per-device data. Only then implement staged HP firmware,
VM, MES/CP, fence and recovery tests. Require a verified GPU write/fence, compute
readback, offscreen rendering/readback, and only subsequently Metal/panel tests
before removing the diagnostic gates. No boot argument bypass is provided.

The existing 780M fallback is preserved and passes mock regression checks. The
baseline has no verified dedicated 740M implementation; synthetic 4-CU validation
must not be described as 740M hardware support. Upstream README performance claims
remain upstream reports about Firebat, not results from this session.
