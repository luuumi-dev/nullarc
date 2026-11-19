# phxfb: isolate VBL delivery using public IOKit timers

Move the primary framebuffer's synthetic VBL callback from default HIGH
thread calls to a public IOTimerEventSource on a dedicated workloop, with
KERNEL priority and no AllowReenter. Preserve absolute staged deadlines,
notifications, missed/late accounting and upstream display-dark behavior.
Stop atomically publishes the stop flag and retires the timer under the same
workloop gate as its action, canceling a rearmed successor before releasing
references. Release partial allocations on failure. Power-on rearming uses
the new API; upstream primary/HDMI cleanup and the HDMI timer are retained.
No private KPI or Info.plist dependency is added.

This ports downstream `439f8a5bc3e086770df7b576897fbb11d7ccc9f1`. Its
[finite corrected-cache boot](https://github.com/AgnoyZ/nullarc/blob/upstream34/vram-layout-fix/docs/validation/build34-vbl-corrected-cache-boot-20261118.json)
removed long sampled VBL plateaus in that trial, but later GUI stutter returned
while VBL stayed responsive. This is not a complete stutter repair; see #31
for the separate AML diagnosis. ASan/UBSan tests use actual callback,
allocation and retirement code with scheduler/clock/gate doubles, including
concurrent action/stop and late rearm. They do not prove XNU behavior or
HDMI/display-power lifecycle acceptance. Apple XNU
`f6217f891ac0bb64f3d375211650a4c1ff8ca1ea` was an API reference; no Apple
implementation was copied. This current-main lifecycle port remains a draft.

This independent source port is based on official main
`3497d87b229e4839d8d2a03ebe9127f2a30f332b`. The current-main port has CPU
coverage only: it has not been built as a driver/plugin, installed, booted or
GPU-tested. Historical downstream hardware evidence does not validate the
additional code on current main. No physical FPS or long-term stability claim
is made. Source rollback is a revert of this focused commit after dependency
review. No deployment occurs in this proposal.

Validation on 2026-11-19: the focused CPU/sanitizer checks listed below passed.

- `python3 tests/test_build34_vbl_scheduling.py`
