# PhxBringup: acquire compute memory before indirect buffers

Compute submissions on the tested Radeon 780M could retire with a wrong
reused-IB result. Emit the downstream-tested eight-dword ACQUIRE_MEM prefix
before compute INDIRECT_BUFFER packets, accounting for its size in ring space.
Graphics packets, completion fences and current upstream VM/lifetime/error
handling retain their behavior. The exact words are `c0065800 00000000 ffffffff
00ffffff 00000000 00000000 0000000a 0000c3b1`; individual cache bits have not
been isolated as independently necessary.

Downstream `62d695424072522b5c05779fe500baec5a73aad5` records two passing guarded
single-IB dispatches after original and equal-footprint NOP controls reproduced
the mismatch. See the [fork evidence](https://github.com/AgnoyZ/nullarc/blob/upstream34/vram-layout-fix/docs/BUILD34-SUBMIT-MEMSYNC.md).
This does not prove every IB/cache ordering case or application behavior.

The new ASan/UBSan test extracts the actual current submit method and checks
20 CPU scenarios: packet values/order, compute/graphics distinction, canonical
addresses, ring wrap, size/space and bind refusal, and fallback engine routing.
Ring memory, VM and doorbells are doubles. Existing native-submit and full
upstream tests cover the preserved failure/ownership behavior.

This independent source port is based on official main
`3497d87b229e4839d8d2a03ebe9127f2a30f332b`. The current-main port has CPU
coverage only: it has not been built as a driver/plugin, installed, booted or
GPU-tested. Historical downstream hardware evidence does not validate the
additional code on current main. No physical FPS or long-term stability claim
is made. Source rollback is a revert of this focused commit after dependency
review. No deployment occurs in this proposal.

Validation on 2026-11-19: the focused CPU/sanitizer checks listed below passed.
The full `make test` attempt is **not green**: upstream's unchanged
`test_wait_seq.cpp` has scheduler-sensitive wall-time assertions. The byte-identical
clean-main control also aborts on this machine. Preserve that failure rather
than disabling assertions. All 16 Python suites run before it passed; the
three remaining queue/pending-fence/HDMI binaries passed independently.
Real firmware fixtures were absent, so firmware-file cases were skipped;
synthetic firmware parsing passed. This is a draft pending full-suite resolution
and native/current-main hardware validation.

- `python3 tests/test_compute_acquire.py`
- `python3 tests/test_native_submit.py`
- `make test`
