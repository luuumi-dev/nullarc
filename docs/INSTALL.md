# Installing nullarc

**760M fork caveat:** the captured HP Envy device is diagnostic-only in this branch.
The upstream installation and Metal checks below describe Firebat 780M releases;
they do not establish HP compatibility. Read [760M_STATUS.md](760M_STATUS.md) first.

**English** · [Русский](INSTALL_RU.md)

nullarc is alpha software that runs in the kernel and inside WindowServer. Keep a way back before you start: a second
bootable macOS or a USB installer, and a copy of your EFI folder.

## What you need

| | |
|---|---|
| GPU | AMD Radeon 780M or 760M (Phoenix, Hawk Point; PCI 1002:15bf, 1900). Only the Firebat U6C (Ryzen 7 H 255) has been tested. |
| macOS | Tahoe 26.6 (tested on 26.6.2, 25G83), installed and booting with OpenCore. |
| OpenCore | A working setup for your machine: Lilu, VirtualSMC and the rest from [`hardware/REQUIRED_KEXTS.md`](../hardware/REQUIRED_KEXTS.md). [`hardware/Sample.plist`](../hardware/Sample.plist) is the reference machine's config with blank serials. |
| Release | `nullarc-<version>.zip` from the [releases page](https://github.com/luuumi-dev/nullarc/releases), unpacked. |

## 1. OpenCore

1. Copy `EFI/OC/Kexts/PhxBringup.kext` from the release into your EFI's `EFI/OC/Kexts/`.
2. In `config.plist`:
   - **Kernel → Add**: an entry with `BundlePath` `PhxBringup.kext`, `ExecutablePath` `Contents/MacOS/PhxBringup`,
     `PlistPath` `Contents/Info.plist`, `MinKernel` `25.0.0`, `Enabled` true. Put it after Lilu and VirtualSMC.
   - **NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82**:
     - `boot-args` must contain `amfi=0x80 amfi_get_out_of_my_way=0x1 -amfipassbeta phxdebug=1` (with AMFIPass.kext in Kernel → Add);
     - `csr-active-config` = `7F080000`.
     Both keys also belong under NVRAM → Delete, so a new value takes effect.
   - **Misc → Security → SecureBootModel** = `Disabled`.
3. Reboot. In Terminal, `ioreg -l | grep phx-build` must print a line: PhxBringup is loaded.

`amfi=0x80` lets WindowServer load the ad-hoc signed driver. It turns library validation off for the whole system,
so FairPlay video (Apple TV+, Netflix in Safari) stops working and some apps that check code signatures may refuse to
run.

## 2. Run the installer

In Terminal, in the unpacked release folder:

```sh
sudo ./install.sh --check          # checks only, changes nothing
sudo ./install.sh --windowserver   # installs; the desktop is composited on the GPU from the second boot on
```

Without `--windowserver` only apps get Metal on the GPU and WindowServer keeps drawing the desktop on the CPU.

The installer puts:

| What | Where |
|---|---|
| Boot-time GPU bring-up (`phxboot.sh`, scripts, `phxctl`, `mesboot`) and the GPU firmware | `/Users/Shared/gpu` |
| LaunchDaemon that runs it at every boot | `/Library/LaunchDaemons/local.phxboot.plist` |
| `PhxAccel.kext`, built into the Auxiliary kernel collection | `/Library/Extensions` |
| Metal plugin `NVMTLDriver.bundle` and Mesa RADV `nvmtl/` | `/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU`, linked from `/Library/GPUBundles` |

The plugin lives under `/System/Library/CoreServices` because sandboxed GPU processes (Safari's, Chromium's) may
load code only from system paths. That folder is a firmlink into the Data volume: the sealed system volume stays
untouched.

## 3. Reboot

- **First boot:** the GPU comes up within a couple of minutes after power-on. With `--windowserver`, turn on automatic
  login (System Settings → Users & Groups) before rebooting: the login window may come up black on the GPU path.
- **With `--windowserver`, reboot once more:** WindowServer starts on the GPU from this boot.

Check:

- `/var/log/phxboot.log` ends with `done`;
- System Information → Graphics/Displays shows **AMD Radeon 780M** with **Metal Support: Metal 3**;
- `sysctl debug.nvaccel_iop_ok` grows while the screen changes (WindowServer frames drawn by the GPU).

The first time a new kind of visual effect appears, its shader is translated and compiled: expect a pause of
1–3 seconds once. It is cached on disk afterwards.

## When something goes wrong

phxboot guards each boot:

- a boot where the bring-up did not finish (hang, power cut) is detected on the next one: that boot leaves the GPU off
  and creates `/Users/Shared/gpu/phxboot-off`;
- if WindowServer crashes or hangs 3 times within 90 seconds on the GPU, phxboot goes back to the firmware framebuffer
  and reboots;
- the bring-up arms a reboot timer that fires if it stalls for 3–4 minutes;
- later in the session, a GPU hang shows up as WindowServer hanging again and again (the session is logged out and the
  screen stays black): after two WindowServer hang reports within 5 minutes phxboot reboots the Mac, and after three
  such reboots within an hour it puts WindowServer back on the firmware framebuffer.

Background neural-network work (Photos analysis `mediaanalysisd`, `photoanalysisd`, `Spotlight`) is kept off the GPU:
its convolution kernels hung the GPU in testing. It runs on the CPU as before.

To turn the GPU off by hand: `sudo touch /Users/Shared/gpu/phxboot-off` and reboot. To turn it on again:
`sudo rm /Users/Shared/gpu/phxboot-off` and reboot. If macOS no longer boots, start without PhxBringup: disable it in
OpenCore (or use your EFI copy), boot, and run the uninstaller.

## Updating

Unpack the new release and run its `install.sh` again (same options), then reboot. Replace `PhxBringup.kext` in your
EFI if the release notes say so.

## Uninstalling

```sh
sudo ./uninstall.sh
```

Then remove `PhxBringup.kext` from OpenCore (the folder and the Kernel → Add entry) and, if nothing else needs them,
the `amfi=0x80 amfi_get_out_of_my_way=0x1 phxdebug=1` boot-args. Reboot.

## Not part of nullarc

- **Audio:** macOS Tahoe no longer ships `AppleHDA.kext`. AppleALC alone gives no sound; AppleHDA has to be restored
  (OpenCore Legacy Patcher does it). nullarc does not redistribute Apple software.
- **Battery, Wi-Fi, Bluetooth:** regular hackintosh kexts, see [`hardware/REQUIRED_KEXTS.md`](../hardware/REQUIRED_KEXTS.md).
  The release's `extras/firebat-u6c/` has the reference laptop's AC-adapter SSDT.
