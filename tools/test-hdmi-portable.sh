#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Compile and run all portable HDMI/DCC/modeset preflight tests.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="$(mktemp -d "${TMPDIR:-/tmp}/xegfx-hdmi-tests.XXXXXX")"
trap 'rm -rf "$DIR"' EXIT
CXX="${CXX:-c++}"
for test in test_hdmi_gmbus test_hdmi_edid_timing test_hdmi_modeset_preflight test_hdmi_mode_plan test_hdmi_native_sequence test_hdmi_c10_msgbus test_hdmi_mtl_power test_hdmi_scanout_abi test_hdmi_c10_pll_image; do
  echo "== $test =="
  "$CXX" -std=c++17 -Wall -Wextra -Werror \
    -I "$REPO/kexts/NMIntelFB" -I "$REPO/gpu/xegfx/shim/include" \
    "$REPO/tools/$test.cpp" -o "$DIR/$test"
  if [[ "$test" == test_hdmi_edid_timing && -n "${HDMI_MONITOR_EDID:-}" ]]; then
    "$DIR/$test" "$HDMI_MONITOR_EDID"
  else
    "$DIR/$test"
  fi
done
echo "All portable HDMI tests passed."
