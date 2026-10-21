#!/usr/bin/env python3
# Usage: tools/kexport.py sym [sym...]  (C names, no leading underscore). Prints which kernel bundle exports each
# symbol to kexts on this Mac, or "-" when none does (then it cannot be called from XeGfx).
import os, sys
sys.path.insert(0, os.path.expanduser("~/src/kcabi"))
import kcfile, linkcheck
e = linkcheck.exported(kcfile.KC("/System/Library/KernelCollections/BootKernelExtensions.kc"))
for s in sys.argv[1:]:
    print(f"{s:44s} {e.get('_' + s, '-')}")
