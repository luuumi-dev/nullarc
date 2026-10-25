# nullarc — agent handoff

Metal and an accelerated WindowServer for the Radeon 780M (Phoenix / Hawk Point, gfx1103) on a macOS Tahoe 26.6.2
hackintosh, the Firebat U6C. README.md says what works; **BUGS.md is the work queue**. Credentials and local paths are in
CLAUDE.local.md (untracked); read it first.

## Rules from the owner (non-negotiable)

- Reply in Russian, short. Post a one-line status often during long work, without stopping to wait.
- Never give the owner a macOS Terminal command. Ask only for GUI clicks, plugging things in or power-cycling; do the
  rest yourself over SSH.
- A test that needs the owner (typing, dragging windows, plugging USB-C, looking at the panel): end the turn with the
  request, start measuring only after they answer «готов».
- Work BUGS.md by severity, S1 first. Every new bug or gap (missing audio counts) goes into the table at once, with a
  severity; update its row when you learn something. Fixed rows move to «Исправлено» with the commit.
- Changes that can stop the Mac from booting (EFI kexts, kernel collections, config.plist, root patches) only while the
  owner is awake and has agreed. Keep the safety nets: one-shot NVRAM arming, phxboot's hang watcher, backups.
- Never osascript / Apple Events over SSH (TCC prompts). No installs and no passwordless sudo on this Linux PC.
- Do not reproduce QEMU / OSX-KVM / Apple SMC key content. The Wi-Fi password lives only in itlwm's Info.plist on the
  EFI, never in git or notes. A GitHub token is given per push and never stored; without one, commit locally.
- Commit messages end with the Co-Authored-By / Claude-Session trailer lines the session provides.

## Machines

- **This PC** (Arch Linux): the repo, git, analysis. No sudo.
- **The Mac** (Firebat U6C, Ryzen 7 H 255): user `dripside`, on the home Wi-Fi (192.168.31.x; AirportItlwm). macOS
  gives the network a private MAC, so find it by its SSH host key: `tools/remote/findmac.sh` (known_hosts in
  `~/.ssh/known_hosts_firebat_mac`). `MAC_PASS=… tools/remote/m.sh script.sh` copies a script there and runs it.
  The shell there is zsh: `log` is a builtin, use `/usr/bin/log`. `sudo` needs `-S` on every call (no tty).
  A TP-Link UE200 USB Ethernet is the wired fallback, but it burns ~1.5 cores while plugged (BUGS.md B-25).
- **Build tree on the Mac**: `~/gpu` (MacKernelSDK, Vulkan-Headers, mesa, and working copies `~/gpu/nmplugin`,
  `~/gpu/phxaccel`). Copy changed sources from the repo there (compare md5 first), build there with CLT clang.

## Layout

| Path | What |
|---|---|
| `src/phxbringup` | PhxBringup.kext (OpenCore-injected): GPU bring-up (PSP, SMU, GFX/MES, GART), the user client RADV talks to. Selectors in `PhxNativeABI.h` (2 READREGS, 102 VRAM read, 108 debug register write, needs `phxdebug=1`). |
| `src/phxaccel` | PhxAccel.kext (Auxiliary KC): NMXAccel accelerator, PhxAMDFB framebuffer (takes the GOP scan-out, VBL timer, display pipe copies frames, backlight PWM handler), PhxAGDC. `build.sh <MacKernelSDK>`. |
| `src/phxhda` | PhxHDA.kext (Lilu plugin, OpenCore-injected after AppleALC): answers AppleHDA's resource requests in the kernel, since kernelmanagerd here serves no kext outside the system KC. `build.sh <MacKernelSDK> <Lilu source>` (`~/gpu/lilu-sdk/Lilu`). |
| `src/nmplugin` | The Metal plugin NVMTLDriver.bundle: Metal on Vulkan (RADV). `build.sh` on the Mac, `tools/phxctl/plugdeploy.sh` installs it. Knobs in `PhxGPU/nvmtl/nvrm610.conf`. |
| `mesa-patches-phoenix` | RADV Darwin patches (Mesa built on the Mac in `~/gpu/mesa`). |
| `tools/phxctl` | Mac-side control: `phxboot.sh` (boot daemon: bring-up, arming, hang watcher), `full.sh`/`bringup5.sh`, `kinst.sh` (rebuild the Aux KC), `plugdeploy.sh`, `phxbl.c` (backlight), `alc_le_paths.py`, `usbcap.sh`. |
| `tools/anim`, `tools/iosurf` | Tests: `scenetest` (GL/SceneKit fps), `typetest` + `scanout_watch` (typed text on the panel), `wintest`, `buftexprobe`, `scanout_dump`, `regpeek`. |
| `tools/remote` | `findmac.sh`, `m.sh` (this PC → Mac). |
| `install`, `tools/release` | `install.sh` / `uninstall.sh`, release builder. |
| `hardware` | ACPI SSDTs, dumps (DSDT, IP discovery, Linux amdgpu register trace). |
| `docs` | Install guides, research and plans. |

## The loop

1. Change the source here. 2. Copy it to the Mac's working copy and build there. 3. Deploy: the plugin with
`plugdeploy.sh` (apps get it at once, WindowServer after a reboot); PhxAccel by copying the kext to
`/Library/Extensions` and running `kinst.sh`. 4. Reboot (`sudo shutdown -r now`), wait with `findmac.sh`.
5. Check: `/var/log/phxboot.log` ends in `done`; WindowServer's plugin log
(`/usr/bin/log show --last 10m --predicate 'process == "WindowServer" AND senderImagePath CONTAINS "NVMTL"'`);
no new WindowServer reports in `/Library/Logs/DiagnosticReports`. 6. Commit, update BUGS.md.

How a boot gets acceleration: NVRAM `phxfb-boot` (one-shot) plus `/Users/Shared/gpu/phxboot-m2` let PhxAMDFB take the
panel; phxboot re-arms after a good boot, so a panic falls back to the firmware framebuffer on the next boot. A GPU hang in
any process takes WindowServer down; phxboot reboots after two WindowServer reports within five minutes, and three such
reboots within an hour turn acceleration off (remove the trigger, then re-arm).

## Pitfalls already paid for

- Disk numbers change between boots: find the ESP from the boot volume's APFS physical store.
- Never delete an EFI kext before its replacement is verified (a failed download once left the EFI without Lilu).
- A reboot while the screen is locked brings auto-login back locked (macOS behaviour, not a bug).
- Apple's neural-network kernels (Espresso, MPSNeuralNetwork) hang the GPU; the plugin refuses them.
- WindowServer runs with real uid 0 / effective uid 88: Mesa's disk cache needed the getuid rebind (df01d18).
- RADV makes no linear image over rows that are not a multiple of 256 bytes; surfaces and buffer textures with such rows
  go through private images the GPU fills and writes back (573959f, ef414d6, 1d93f66, ee2a718).

## Where things stand

BUGS.md has the full list. Staged on the Mac, waiting for the owner (they could not test overnight):
- Typed-text fix (B-05) is deployed: ask them to type in Spotlight and Messages.
- Brightness (B-04): `phxbl <percent>` writes the PWM; ask whether the panel dims. Then install
  `~/gpu/phxaccel-bl/build/PhxAccel.kext` (brightness slider, `-phxbloff` disables) and BrightnessKeys (`~/gpu/bk`).
- Audio (B-03): `~/gpu/alc/AppleALC.kext` has its AppleHDA paths moved to `/Library/Extensions`; swap it into
  `EFI/OC/Kexts` with a backup, then reboot.
- USB-C lag (B-01, S1): run `usbcap.sh 60` in the background, then ask them to plug the device in.
