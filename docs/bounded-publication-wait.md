# nmplugin: bound opt-in IOSurface publication waits

This is an alternative to #28's unbounded writer commit wait, not an
extra wait to merge on top. Credit QiRanChenk for #28's writer classifier
(head `317092664427f3fe7650f5fa08e8fe1b7ba011df`); deadline/concurrency changes
come from downstream `52d68e68731aead50eda41ce427d4c5112659d4c`.

Capture surface writers under the submission lock before dispatch, avoiding
post-submit iteration of mutable parts. Signal a separate dispatch group after
completion/publication bookkeeping, excluding user-handler execution. Use
dispatch-specific execution-queue identity, including target inheritance.
Exact `NVMTL_SURFACE_COMMIT_WAIT=1` opts in; WindowServer stays excluded and
the kill switch wins. A monotonic 250 ms default budget accepts strict decimal
1..2000 overrides. Measure actual elapsed time with CLOCK_UPTIME_RAW and
rate-limit timeout diagnostics. Expiry keeps pending state/resources and
leaves publication unconfirmed; it neither frees work nor forges success.

This deliberately changes #28's unconditional publication guarantee: commit
can return on expiry with work pending. A consumer needing pixels must check
successful completion before handoff. This policy needs upstream review,
so the proposal is a draft. Ten ASan/UBSan CPU tests cover controls/bounds,
no-surface paths, publication, pending handlers, expiry/late completion,
elapsed samples, queue/target identity, label collision, multiple waiters,
terminal error preservation and wiring. The [fork record](https://github.com/AgnoyZ/nullarc/blob/upstream34/vram-layout-fix/docs/BUILD34-PR28-WAIT-TIMING.md)
separates historical CPU, GPU and owner-selected daily configuration evidence.
This source proposal changes no installed configuration.

This independent source port is based on official main
`3497d87b229e4839d8d2a03ebe9127f2a30f332b`. The current-main port has CPU
coverage only: it has not been built as a driver/plugin, installed, booted or
GPU-tested. Historical downstream hardware evidence does not validate the
additional code on current main. No physical FPS or long-term stability claim
is made. Source rollback is a revert of this focused commit after dependency
review. No deployment occurs in this proposal.

Validation on 2026-11-19: the focused CPU/sanitizer checks listed below passed.

- `make test-nmplugin-surface-commit-wait`
