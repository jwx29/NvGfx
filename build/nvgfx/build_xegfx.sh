#!/bin/bash
# Builds XeGfx.kext (Intel Xe, gpu/xegfx) into gpu/xegfx/out: every drm/i915/LinuxKPI object, then the link with the
# KPI, shadow and kcabi link checks. Sources: ~/src/xe-src (linux, freebsd, drm-kmod), firmware ~/src/xe-src/linux-firmware.
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"; X="$R/gpu/xegfx"
export XEGFX_OVERLAY="$R/gpu/xkpi"
python3 "$X/harness/score.py" --obj > "$X/out/score.log" 2>&1 || { tail -20 "$X/out/score.log"; exit 1; }
# a source that did not compile would be linked as panicking stubs (N-223 died in one), so stop here instead
read -r ok all < <(sed -nE 's/^total +([0-9]+)\/ *([0-9]+).*/\1 \2/p' "$X/out/score.log")
[ -n "${ok:-}" ] && [ "$ok" = "$all" ] || {
  echo "STOP: ${ok:-?}/${all:-?} sources compiled"
  grep -A20 "failed sources:" "$X/out/score.log" | head -75 || true
  if [ -s "$X/out/compile-failures.log" ]; then
    echo "Compiler diagnostics (full report: $X/out/compile-failures.log):"
    sed -n '1,100p' "$X/out/compile-failures.log"
  fi
  exit 1
}
bash "$X/tools/link.sh"
