# IOSurface synchronization key regression test

On macOS, run `make test-nmplugin-surface-key` (Xcode command-line tools and Python 3).
The runner compiles the actual production helper and extracts the pending-writer
registry and synchronization-monitor functions into a CPU test with mock surface
IDs/seeds and minimal Objective-C objects. Vulkan and GPU code are not executed.
The test uses ASan/UBSan, checks 77 boundary key identities (including full-width
planes), replacement and weak lifetime behavior, backing/seed guards, monitor
coalescing and release, and 16,384 concurrent registry/monitor cycles.
Generated source and executables stay under `build/tests/surface_key`.
These checks do not measure desktop FPS or prove driver hardware acceptance.

## Optional isolated hardware probe

With a compatible accelerator already running, compile:

```sh
clang -O1 -g -fobjc-arc -Wall -Wextra -Werror tools/iosurf/surface_sync_probe.m \
  -framework Foundation -framework Metal -framework IOSurface -framework IOKit \
  -o build/tests/surface_sync_probe
build/tests/surface_sync_probe /absolute/path/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver
NVMTL_NO_SURFACE_HOST=1 build/tests/surface_sync_probe /absolute/path/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver
build/tests/surface_sync_probe /absolute/path/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver planar
```

The probe explicitly loads the selected plugin in its own process, uses the
existing NMXAccel service and checks same-surface aliases, outstanding ordered
writers, GPU readback, CPU locks and two separate NV12 planes. It does not install
or activate that plugin system-wide. Release constructors clear inherited tuning
variables; the probe restores only its explicit `NVMTL_NO_SURFACE_HOST` control
before device creation. Cross-queue reads keep explicit completion dependencies.
