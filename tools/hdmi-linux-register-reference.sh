#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Read-only i915 reference capture for Intel Meteor Lake DDI B HDMI.
# Default reads existing Linux DRM debugfs state only. Raw MMIO is opt-in.
set -euo pipefail

if [[ ${1:-} != "" && ${1:-} != "--i-understand-mmio-risk" ]]; then
    echo "Usage: $0 [--i-understand-mmio-risk]" >&2
    exit 2
fi

GPU=/sys/kernel/debug/dri/0000:00:02.0
if [[ ! -d $GPU ]]; then
    echo "GPU debugfs directory not found: $GPU" >&2
    echo "Mount debugfs or confirm the Intel GPU PCI address before continuing." >&2
    exit 1
fi

echo "# Linux i915 HDMI DDI B reference (with HDMI actively displaying 1080p60)"
echo "# Date: $(date -u -Is)"
if command -v lspci >/dev/null 2>&1; then
    echo "# PCI: $(lspci -nn -s 00:02.0)"
fi

capture() {
    local label=$1
    shift
    echo
    echo "### $label"
    local f
    for f in "$@"; do
        if sudo test -r "$GPU/$f"; then
            echo "# Source: $GPU/$f"
            sudo cat "$GPU/$f"
            return 0
        fi
    done
    echo "# Unavailable: $*" >&2
    return 0
}

capture "Active display pipes, transcoders and connectors" \
    i915_display_info
capture "Shared PLL allocation and clocks" \
    i915_shared_dplls_info i915_shared_dplls
capture "Display power domains" \
    i915_power_domain_info

echo
echo "### HDMI connector"
for con in /sys/class/drm/card*-HDMI-A-*; do
    [[ -d $con ]] || continue
    echo "# $con"
    [[ ! -r $con/status ]] || cat "$con/status"
    [[ ! -r $con/modes ]] || head -n 12 "$con/modes"
    [[ ! -r $con/ddc/name ]] || cat "$con/ddc/name"
done

if [[ ${1:-} != "--i-understand-mmio-risk" ]]; then
    echo
    echo "# Raw register reads skipped (default). No MMIO writes performed."
    exit 0
fi

if ! command -v intel_reg >/dev/null 2>&1; then
    echo "Install intel_reg (intel-gpu-tools / igt-gpu-tools) for optional raw reads." >&2
    exit 1
fi

echo
echo "### Optional raw display registers"
echo "# Warning: raw MMIO reads may hang a GPU even though they do not write."
echo "# Never read GMBUS2 at 0xC5108; its INUSE semaphore is read-to-acquire."
sudo intel_reg --pci-slot=0000:00:02.0 read \
    0x45400 0x45404 0x45450 0x45454 \
    0x46000 0x46104 0x46144 \
    0x64000 0x64100 0x60400 0x61400 \
    0x70008 0x71008 0x70180 0x71180 \
    0x7019c 0x7119c 0x60000 0x61000 \
    0x61008 0x6100c 0x61014
