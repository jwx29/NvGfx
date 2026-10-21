#!/usr/bin/env python3
# Splits out/undefined.txt into symbols the macOS kernel already exports (with the bundle that exports them) and the
# ones XeGfx must implement itself. Uses Bash's kcabi reader on the boot kernel collection.
import os, sys
sys.path.insert(0, os.path.expanduser("~/src/kcabi"))
import kcfile, linkcheck
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
kc = kcfile.KC("/System/Library/KernelCollections/BootKernelExtensions.kc")
exp = linkcheck.exported(kc)
have, todo = [], []
for line in open(os.path.join(ROOT, "out/undefined.txt")):
    n, sym = line.split()
    (have if sym in exp else todo).append((int(n), sym, exp.get(sym, "")))
open(os.path.join(ROOT, "out/kpi-have.txt"), "w").write("".join(f"{s} {o}\n" for _, s, o in have))
open(os.path.join(ROOT, "out/kpi-todo.txt"), "w").write("".join(f"{n} {s}\n" for n, s, _ in todo))
print(f"exported by macOS: {len(have)}   to implement: {len(todo)}")
