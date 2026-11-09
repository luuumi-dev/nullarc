# Fixed panel timing regression test

Run from `src/phxaccel` on macOS:

```sh
clang++ -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/panel_timing.cpp -o /tmp/phx-panel-timing-tests
/tmp/phx-panel-timing-tests
```

The fixture is a read-only OTG0 snapshot of a Radeon 780M (1002:1900),
2560x1600 at 120 Hz, captured on November 7, 2026. Expected totals are
2720x1694, H sync offset/width 48/32, V sync offset/width 3/6, negative
sync polarity on both axes. Estimated pixel clock: 552,921,600 Hz.

Tests cover valid and fractional refresh, sync polarity, mismatched console
geometry, unstable snapshots, disabled/interlaced/variable modes, invalid
register fields, sync outside blanking, and maximum bounded clock arithmetic.
They do not load a kernel extension or access hardware.

The framebuffer publishes detailed timing only after validation; otherwise
its prior no-detailed-timing behavior is preserved. The pixel clock is derived
from the existing measured/snapped refresh, not from a hardware clock divider.
Clock error bounds remain unspecified (zero), so the change does not opt into
IOFramebuffer's clock-based VBL throttling. It does not add VRR, change display
registers, alter VBL scheduling, or remove GPU completion waits.

A successful test/build is not evidence of better desktop FPS. A controlled
boot and comparable desktop measurements are still required before deployment.
