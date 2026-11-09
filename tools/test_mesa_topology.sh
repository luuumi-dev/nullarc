#!/bin/sh
# Run after applying the complete Mesa patch series. Host-only: no GPU access.
set -eu
MESA=$(cd "${1:?usage: test_mesa_topology.sh <patched-Mesa-tree>}" && pwd)
REPO=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$REPO/build/tests"
for source in "$REPO/tests/test_mesa_phx_topology.c" "$MESA/src/amd/common/darwin/test/acd_kext_test.c"; do
    out="$REPO/build/tests/$(basename "$source" .c)"
    clang -std=gnu11 -I"$MESA/include" -I"$MESA/include/darwin" -I"$MESA/src/amd/common/darwin" \
        "$source" "$MESA/src/amd/common/darwin/ac_darwin_kext.c" -o "$out"
    "$out"
done
