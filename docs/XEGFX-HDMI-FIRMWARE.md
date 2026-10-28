# Meteor Lake HDMI status and firmware-primary output trial

## Current scope (important)

XeGfx currently has ONE `NMIntelFB` framebuffer (`getConnectionCount() == 1`),
ONE firmware scanout and ONE accelerator panel (head 4). This is not a native
i915 HDMI modesetting implementation. There is no functioning macOS EDID/DDC,
HDMI hotplug, HDMI PHY/PLL link training or independent second desktop yet.

This change improves **firmware-initialised external display takeover**:
the framebuffer probes all four lit Intel pipes, records SHOTPLUG_CTL_DDI/TC
(read-only; values alone don't identify an HDMI connector), and prefers the
single validated linear XRGB8888 pipe matching the GOP console size/stride.
It does not write display registers. The original first-pipe fallback is
preserved if two pipes have identical geometry, but that case is ambiguous.

A real HDMI connector implementation needs Linux i915 display connector/
encoder, DDC/EDID, HPD, link/mode programming, IOFramebuffer multi-head
registration, AGDC per-head mapping, accelerator head count/pipe binding,
surface lifetime and scanout sync. Do not just change getConnectionCount() to
2 or invent an IOFBDependentIndex: that would expose a fake screen.

## Firmware HDMI-primary test (only when firmware supports it)

1. Retain a bootable USB/EFI with the old, working display stack.
2. Connect the HDMI screen *before powering on*, and check firmware setup
   for a primary/external-display/boot-video setting. Use it only if present.
   You may also test lid-closed boot on firmware that explicitly supports it,
   but don't rely on that to prove HDMI routing.
3. The monitor MUST already show the actual firmware/boot console before
   testing the driver. If it remains black through OpenCore, this patch cannot
   initialize it, because there is no link-training or modesetting.
4. Build/install with your normal `build_xegfx_stack.sh` / `install.sh` flow,
   then reboot with the HDMI monitor already connected. Don't enable new
   boot-args or arbitrary framebuffer connector patches.
5. Check the chosen pipe and scanout log:

```sh
sudo log show --last 30m --style compact --predicate 'eventMessage CONTAINS "NMIntelFB:"' | tail -100
ioreg -p IOService -l -w0 -c NMIntelFB | grep -E 'NMIntelFB(Pipe|HPD|FirmwarePipeMatches)|IOFBDependent|IOFBMemorySize'
sudo system_profiler SPDisplaysDataType
```

An HPD status bit alone does not establish an HDMI display. Even when the
external display has the boot console, this proof-of-concept may refuse to
start if the GOP display is tiled, console stride differs, or the selected
plane is not uniquely identifiable.

## Physical wiring diagnostics (Linux recommended)

Boot a Linux live USB with the same HDMI monitor connected. This is a
hardware-detection step, not a driver installation.

```sh
lspci -nnk | grep -A5 -Ei 'VGA|Display'
ls -l /sys/class/drm
for x in /sys/class/drm/card*-HDMI-A-*/status /sys/class/drm/card*-DP-*/status; do
  [ -r "$x" ] && printf '%s: %s\n' "$x" "$(cat "$x")"
done
sudo dmesg | grep -Ei 'i915|xe |drm.*(hdmi|hpd|ddi|connector|edid)' | tail -120
```

For the specific HDMI connector, record `connected`, its EDID-supported
modes (e.g. `cat /sys/class/drm/card*-HDMI-A-1/modes`) and which GPU bound
the connector. If HDMI is not detected by Linux's Intel DRM driver, check
physical port routing/retimers/BIOS before extending XeGfx.

## Recover

If boot fails, use the retained working EFI and restore the prior kexts and
AuxKC. `-nmintelfboff` disables NMIntelFB for a fallback boot (depending on
how your EFI loads the firmware framebuffer). No connector-specific boot-arg
is required for the firmware-only trial.

Reference: Linux i915 `intel_display_regs.h` for SHOTPLUG_CTL_DDI (0xC4030)
and SHOTPLUG_CTL_TC (0xC4034), and the upstream XeGfx firmware scanout design.
