# phxfb: publish validated panel timings with bounded refresh sampling

Publish detailed fixed-panel timing only when paired read-only OTG0
snapshots are stable, progressive, enabled and consistent with active geometry
and valid sync/blanking ranges. Keep the old no-detailed-timing fallback on
failure. Pixel clock is estimated from measured/snapped refresh and totals;
unknown clock-error bounds remain zero.

This supersedes closed draft #9 with downstream corrected A3 (`d40d2ec` plus
`4279b23`). A 200 ms frame-counter sample could quantize 120 Hz to about
124 Hz. The bounded window is 2 s, retaining the existing validity checks,
adding about 1.8 s to startup measurement. This is not a boot-speed change.
No display register is written. ASan/UBSan cases cover the decoder and actual
measurement/publication methods with MMIO/clock doubles, including counter
wrap, quantization, changed/interlaced/variable timing, polarity and fallback.
See the [corrected-A3 evidence](https://github.com/AgnoyZ/nullarc/blob/upstream34/vram-layout-fix/docs/UPSTREAM34-BACKPORT.md).
Native SDK, boot, mode publication and sleep/wake validation remain for this
current-main port, so this proposal is a draft.

This independent source port is based on official main
`3497d87b229e4839d8d2a03ebe9127f2a30f332b`. The current-main port has CPU
coverage only: it has not been built as a driver/plugin, installed, booted or
GPU-tested. Historical downstream hardware evidence does not validate the
additional code on current main. No physical FPS or long-term stability claim
is made. Source rollback is a revert of this focused commit after dependency
review. No deployment occurs in this proposal.

Validation on 2026-11-19: the focused CPU/sanitizer checks listed below passed.

- `make test-panel-timing`

## Integration on 2026-11-27

Ported PR #35 onto the existing sleep candidate while retaining the early
OTG-disabled rejection. The local source had a 50 ms window and 4% snapping:
a CPU replay of 120 Hz produced 137349 mHz, and 50 Hz could snap to 48 Hz.
The window is now 2 s and snapping uses 2%. Timing validity is cleared before
each measurement, so an unsuccessful repeat cannot retain old detailed timing.
The 43 decoder cases and 511 callback assertions pass under ASan/UBSan.
These are CPU tests; this revision still needs a native rebuild and live timing
publication checks. It does not implement OLED AUX brightness or scaled modes.

Publication scope: only panel timing changes are included here; the local sleep, experimental HDMI and NVRAM modifications are excluded. Native compilation of the final publication tree is checked separately.
