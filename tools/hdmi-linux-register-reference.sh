#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Read-only reference capture of Linux i915's fully working DDI B HDMI
# state. Run with an HDMI monitor ACTIVE at 1920x1080 60 Hz in Nobara.
# Requires intel_reg (intel-gpu-tools / igt-gpu-tools).
# No register writes, but low-level MMIO reads can still be disruptive.
set -euo pipefail
if ! command -v intel_reg >/dev/null; then
  echo "Missing intel_reg (install intel-gpu-tools or igt-gpu-tools)" >&2
  exit 1
fi
if [ "${1:-}" != "--i-understand-mmio-risk" ]; then
  echo "Usage: $0 --i-understand-mmio-risk" >&2
  echo "Warning: raw Intel MMIO reads can hang hardware; only run on the working Linux i915 setup." >&2
  exit 2
fi
echo "# Intel Meteor Lake 8086:7d55 HDMI B reference, HDMI active under Linux"
echo "# UTC $(date -u -Is)"
echo "# PCI $(lspci -nn -s 00:02.0)"
echo "# NOTE: capture raw registers ONLY, NO writes"
# Each register is verified in intel_display_regs.h, no GMBUS2 semaphore reads.
# The --pci-slot flag keeps the tool on the physical Intel GPU.
sudo intel_reg --pci-slot=0000:00:02.0 read \
  0x45400 0x45404 0x45450 0x45454 \
  0x46000 0x46104 0x46144 \
  0x64000 0x64100 0x60400 0x61400 \
  0x70008 0x71008 0x70180 0x71180 \
  0x7019c 0x7119c 0x60000 0x61000 \
  0x61008 0x6100c 0x61014
