# nmplugin: exclude framebuffer-fetch bindings from descriptor masks

Framebuffer-fetch set 3 is bound separately. Including it in ordinary
stage masks can invalidate them and force full descriptor snapshots. Add
separate masks excluding sets 2/3 and prefer valid optimized masks, preserving
legacy masks, conservative fallbacks, tessellation and `NVMTL_DESC_MASK=0`.
This is the unadopted part of closed #8 on current main; all newer upstream
rendering-error fixes are retained. Attribution to NullMoth
`18ba05f38b5eadea8b5ae05834bb51c61fb4c5c3` and PolyForm notices remain.

The actual parser, initialization expressions and descriptor writer run under
ASan/UBSan in unset/0/1 modes, with Vulkan endpoints doubled. Clang, Python
and Vulkan headers are required; see `tests/README-nmplugin-descriptor-mask.md`.
Historical isolated GPU results in #8 apply to the older base, not this port.
Synthetic descriptor counts are not a live FPS measurement.

This independent source port is based on official main
`3497d87b229e4839d8d2a03ebe9127f2a30f332b`. The current-main port has CPU
coverage only: it has not been built as a driver/plugin, installed, booted or
GPU-tested. Historical downstream hardware evidence does not validate the
additional code on current main. No physical FPS or long-term stability claim
is made. Source rollback is a revert of this focused commit after dependency
review. No deployment occurs in this proposal.

Validation on 2026-11-19: the focused CPU/sanitizer checks listed below passed.

- `make test-nmplugin-descriptor-mask`
