# nmplugin: share compact IOSurface keys with synchronization monitors

Current main already packs ordinary pending-writer keys, while per-plane
synchronization still creates NSArray keys. Share a compact helper between
both paths: NSNumber contains `(uint64_t(surfaceID) << 8) | plane` for planes
0..255; larger full-width planes keep decimal string keys. The classes are
disjoint. Locks, weak references, seed/backing guards and waits are preserved.

This submits only the remaining part of closed #7. Sampler keys from #6 are
already adopted, and the surface texture cache has a separate existing #16.
Production key/registry/monitor code runs under ASan/UBSan with CPU IOSurface
doubles: boundary uniqueness, fallback, stale seed/backing rejection,
replacement/completion/weak lifetime, monitor identity and concurrent cycles.
See `tests/README-nmplugin-surface-key.md`. GPU evidence in #7 is historical
and does not validate this current-main port.

This independent source port is based on official main
`3497d87b229e4839d8d2a03ebe9127f2a30f332b`. The current-main port has CPU
coverage only: it has not been built as a driver/plugin, installed, booted or
GPU-tested. Historical downstream hardware evidence does not validate the
additional code on current main. No physical FPS or long-term stability claim
is made. Source rollback is a revert of this focused commit after dependency
review. No deployment occurs in this proposal.

Validation on 2026-11-19: the focused CPU/sanitizer checks listed below passed.

- `make test-nmplugin-surface-key`
