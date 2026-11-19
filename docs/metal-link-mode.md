# phxctl: preserve ordinary-user Metal bundle lookup

With root umask 077, relocation can create the Metal bundle symlink with
mode 0700, blocking ordinary-user principal-class lookup on macOS even when
the executable is readable. Set 0755 using `chmod -h` after checking the exact
expected link destination, both on creation and on repeat relocation. Do not
chmod the target or the separate runtime alias.

This ports downstream `a61336393c8d1182af531abb8b544510a8a5765a`; its
[record](https://github.com/AgnoyZ/nullarc/blob/upstream34/vram-layout-fix/docs/BUILD34-FPS-VALIDATION.md)
separates bundle discovery from later GPU/FPS evidence. Four macOS fixture
tests execute the actual script with only its directory assignments redirected
and codesign replaced by a check: new/existing aliases, restrictive umask,
idempotence, target byte/mode/inode preservation, and unexpected-target refusal.
No system bundle is moved by these tests.

This independent source port is based on official main
`3497d87b229e4839d8d2a03ebe9127f2a30f332b`. The current-main port has CPU
coverage only: it has not been built as a driver/plugin, installed, booted or
GPU-tested. Historical downstream hardware evidence does not validate the
additional code on current main. No physical FPS or long-term stability claim
is made. Source rollback is a revert of this focused commit after dependency
review. No deployment occurs in this proposal.

Validation on 2026-11-19: the focused CPU/sanitizer checks listed below passed.

- `python3 tests/test_gpubundle_link_mode.py`
- `bash -n tools/phxctl/relocate_gpubundle.sh`
