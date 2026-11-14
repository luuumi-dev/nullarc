# Sleep on Phoenix laptops: hibernation (S4)

Phoenix / Hawk Point laptops ship with Modern Standby only. The firmware has no working S3 (the DSDT carries the S3
package under the name `XS3`, and a forced S3 never wakes). What does work is hibernation: macOS writes the memory
image, the machine powers off, and the next power-on restores the session, GPU included.

Verified on a MECHREVO WUJIE15XA (Ryzen 7 8745HS, Radeon 780M, macOS 26.7): sleep from the Apple menu or by closing
the lid, power button to wake, the desktop is back with every window about 2 s after the kernel has resumed.

## What each piece does

| Piece | Why |
|---|---|
| ACPI patch `XS3_` -> `_S3_` (DSDT, count 1) | Without an `_S3` object the platform expert reports "sleep unsupported" and every sleep request ends in `System sleep prevented by kPMPCIUnsupported`. S3 itself is never entered. |
| `PhxSleepPolicy.kext` (src/sleeppolicy), injected right after Lilu | X86PlatformPlugin's sleep policy answers "deep idle" (S3) even with `hibernatemode 25`. `IOPMrootDomain` takes one policy handler, first come first served; this one answers "hibernate" when hibernation is on and aborts the sleep otherwise. It also copies the hibernation key (`IOHibernateRTCVariables`) to NVRAM from its own `setPowerState(0)`: on a PC nothing stores it, and `boot.efi` then fails with `EB|H:FAIL`. |
| OpenCore `Misc > Boot > HibernateMode = Auto`, `HibernateSkipsPicker = true` | OpenCore hands the key to `boot.efi` and goes straight to the wake. |
| `pmset -a hibernatemode 25 standby 0 autopoweroff 0 powernap 0` | Hibernate at once; nothing else is supported. |
| `PhxSleepPolicy.kext`, two seconds after every wake | Evaluates the embedded controller's `_REG(3, 1)` again (the EC is found by its `_HID`, PNP0C09). The EC has been through a power cycle and is back in its non-ACPI mode: it raises no queries, so a second lid close does nothing and the brightness keys are dead, while keys the keyboard controller reports itself still work. On the WUJIE15XA the firmware repeats `XSEC = OSEC` only in `_WAK(4)`, which macOS does not pass on this path. `debug.phxsleep_ecreg` counts the evaluations, `debug.phxsleep_ecreg_last` is the last result, writing 1 to `debug.phxsleep_ecreg_now` runs it by hand. |
| PhxBringup, PhxAMDFB, NMXAccel (this branch) | Hold GPU submissions over the sleep, drop stale VMID bindings, open IOFramebuffer's paging gate, close the display machine's pending mode change. |
| `tools/phxwake` (`phxwaked` + `phxwake.sh`, LaunchDaemon) | After every wake: run the GPU bring-up again when `GRBM_STATUS` reads 0, then `debug.phxfb_wake=1` and `debug.nvaccelfb=9`. |

## Order of events

Sleep: priority notifications -> `IOHibernateSystemSleep()` creates the key -> drivers power down (PhxBringup drains and
holds submissions; PhxSleepPolicy copies the key) -> `IOHibernateSystemHasSlept()` stores `boot-image` and syncs NVRAM ->
image written -> power off.

Wake: OpenCore -> `boot.efi` restores the image -> kernel resumes, GPU in its power-on state, WindowServer waits in
IOFramebuffer's gate and Metal clients wait in `gpuSubmit()` -> `phxwaked` runs the bring-up (about 3 s) -> queues adopted,
submissions released -> gate opened, accelerator enabled -> the desktop is drawn again.

## Limits and notes

- Waking needs the power button (or lid plus power button); it is a cold start of the firmware.
- Do not test with `-v` or `debug=0x100`: the machine hung on the way into sleep with them.
- With the GPU switched off (`phxboot-off`) the boot display `.Display_boot` prevents system sleep altogether.
- HibernationFixup 1.5.4 did not store the key on macOS 26.7; it is not needed with PhxSleepPolicy.
- A sleep that fails half-way can leave an MT7922 Wi-Fi/Bluetooth card dead until the machine has been fully drained
  (shut down, unplug, hold the power button for 20 s). Reboots alone do not reset it.
- USB controllers come back with MSI enabled; anything that changed their interrupt mode has to redo it after the wake.
