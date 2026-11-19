# phxfb: validate pool releases and retry unpublished scanout aliases

Early presents can spend all three alias-verification attempts before
PhxBringup publishes the carve-out. Count unpublished metadata as a separate
bounded wait (3600 presents); retain three mapping/pixel attempts and BAR
fallback unless every sample matches with at least one nonzero sample.
Also validate pool releases using integer addresses and subtraction-safe
page/range checks, and try smaller pools after suballocator creation failure.

This ports downstream A2 `79c0a0f486cc9e0e4bbbde80916cc495b9043127` without its
unrelated device/queue policy helpers. ASan/UBSan tests execute actual
pool/alias methods with IOKit doubles, including allocation failures, missing
metadata, mismatch/zero/match samples, busy exclusion and invalid releases.
Small ranges and boundary arithmetic are checked separately. Historical
hardware results are in the [A2 record](https://github.com/AgnoyZ/nullarc/blob/upstream34/vram-layout-fix/docs/UPSTREAM34-BACKPORT.md).
The historical BAR copy time is not a measured speedup of this port. These
extent guards do not establish general handle provenance.

This independent source port is based on official main
`3497d87b229e4839d8d2a03ebe9127f2a30f332b`. The current-main port has CPU
coverage only: it has not been built as a driver/plugin, installed, booted or
GPU-tested. Historical downstream hardware evidence does not validate the
additional code on current main. No physical FPS or long-term stability claim
is made. Source rollback is a revert of this focused commit after dependency
review. No deployment occurs in this proposal.

Validation on 2026-11-19: the focused CPU/sanitizer checks listed below passed.

- `make test-framebuffer-pool-alias`
