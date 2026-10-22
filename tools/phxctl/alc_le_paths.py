#!/usr/bin/env python3
# alc_le_paths.py <AppleALC binary>: points AppleALC's AppleHDA paths at /Library/Extensions, in place.
# Tahoe has no AppleHDA, so ours sits in /Library/Extensions and loads from the Auxiliary KC. For a kext outside the
# Boot/System KCs Lilu reads the symbols from the file at the path the plugin names, and AppleALC names
# /System/Library/Extensions/AppleHDA.kext/...: no file, no symbols, so AppleHDA was never patched (no layout, no
# PathMaps, AppleHDADriver never registered). Dropping the "/System" prefix keeps each string where it starts, so the
# plugin's pointers to it stay valid; the freed tail is zeroed.
import sys

path = sys.argv[1]
data = bytearray(open(path, "rb").read())
old, n, i = b"/System/Library/Extensions/AppleHDA.kext/", 0, 0
while (i := data.find(old, i)) >= 0:
    end = data.index(b"\0", i)
    s = bytes(data[i:end]); new = s[len(b"/System"):]
    data[i:end] = new + b"\0" * (len(s) - len(new))
    print("patched:", new.decode()); n += 1; i = end
open(path, "wb").write(data)
print(n, "paths")
