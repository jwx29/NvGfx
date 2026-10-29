# Verified Lenovo Meteor Lake iGPU HDMI topology

Linux data supplied 2026-10-29, Nobara Linux kernel 7.2.9, Intel i915 DRM:

- GPU: Intel `8086:7d55` rev 08, Lenovo subdevice `17aa:3813`.
- i915 drives both connectors on the SAME PCI graphics device `00:02.0`.
- `card1-eDP-1`: connected/enabled, 1920×1200, 256-byte EDID.
- `card1-HDMI-A-1`: connected/enabled, 1920×1080 and other modes,
  256-byte EDID.
- `card1-DP-1` through `card1-DP-4`: disconnected.
- The Linux diagnostic had no available `i915_display_info` output, so
  the exact DDI port, encoder, CRTC and active transcoder remain unknown.

This confirms HDMI is physically driven by Intel i915, **not** a discrete
GPU. It does *not* prove which DDI/HPD registers, retimers, PLLs, and PHYs
a macOS driver must initialise. The connector number HDMI-A-1 is a Linux
enumeration, not a hardware port ID.

**The firmware only initialises the internal panel.** Thus an
`NMIntelFB` firmware-console takeover cannot light HDMI, which needs
native i915 KMS or a correctly integrated Intel modesetting bridge.

### Key blockers in the XeGfx branch

- `gpu/xegfx/shim/src/xnu/xg_kext.cpp` sets
  `compat.linuxkpi.i915_disable_display=1`.
- `gpu/xkpi/src/i915/intel_display_device.c` additionally routes
  `compat.linuxkpi.i915_xegfx_no_display=1` through `no_display`,
  preventing i915 from even creating display connectors.
- `kexts/NMIntelFB/intelfb.cpp` exposes one
  `getConnectionCount() == 1` firmware-backed IOFramebuffer with
  one startup mode. It has no EDID/DDC, HPD, HDMI modeset, display
  hotplug, independent output framebuffer or second NVAccel pipe.
- XeGfx/ANV Vulkan rendering works but is not an HDMI output driver.
- Disabling both i915 guards with no ownership coordination risks
  resetting the functional internal eDP panel and its firmware scanout.

### Linux evidence needed for a safe port

The updated `tools/hdmi-linux-probe.sh` now also reads Linux DRM
`/sys/kernel/debug/dri/*/state` and optionally runs `modetest -M i915`
or `drm_info`. These show connector → encoder → CRTC/pipe associations
and the running mode. They do not modify DRM state.

```sh
sudo mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || true
sudo bash tools/hdmi-linux-probe.sh > hdmi-linux-details.txt
```

Test with HDMI connected and active. No OpenCore connector patch is
required for evidence collection.

### Proposed native implementation stages

1. Isolate i915 **read-only display discovery** from the normal
   modeset/ownership path; avoid i915 power/PLL/pipe writes during discovery.
2. Parse Intel VBT/BDB and query the grounded HDMI DDI, HPD, and EDID
   with safe power-domain handling; use Linux connector evidence as a
   reference.
3. Make exclusive ownership of the *new* display pipe and transcoder
   while preserving firmware eDP A, including CDCLK/shared DPLL
   allocation, power wells, DDI PHY, audio/infoframes and hotplug.
4. Add a second independent IOFramebuffer with its own scanout
   allocation, timing, EDID, connect interrupt, VBL, lifecycle and
   matching NVAccel/AGDC head routing.
5. Verify plug/unplug, safe rollback, standby/wake and WindowServer
   restart before making the native HDMI path default.

No active HDMI modesetting is implemented or enabled by this diagnostic commit.
