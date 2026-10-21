#!/bin/bash
# Builds XeGfx.kext (Intel Xe, gpu/xegfx) into gpu/xegfx/out: every drm/i915/LinuxKPI object, then the link with the
# KPI, shadow and kcabi link checks. Sources: ~/src/xe-src (linux, freebsd, drm-kmod), firmware ~/src/xe-src/linux-firmware.
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"; X="$R/gpu/xegfx"
export XEGFX_OVERLAY="$R/gpu/xkpi"
python3 "$X/harness/score.py" --obj > "$X/out/score.log" 2>&1 || { tail -20 "$X/out/score.log"; exit 1; }
bash "$X/tools/link.sh"
