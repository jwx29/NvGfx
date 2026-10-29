# Verified Lenovo Meteor Lake iGPU HDMI topology

Linux data supplied 2026-10-29, Nobara Linux kernel 7.2.9, Intel i915 DRM:

- GPU: Intel `8086:7d55` rev 08, Lenovo subdevice `17aa:3813`.
- i915 drives both connectors on the SAME PCI graphics device `00:02.0`.
- `card1-eDP-1`: connected/enabled, 1920×1200, 256-byte EDID.
- `card1-HDMI-A-1`: connected/enabled, 1920×1080 and other modes,
  256-byte EDID.
- `card1-DP-1` through `card1-DP-4`: disconnected.
- **eDP-1 uses DDI A / PHY A, CRTC pipe A**, 1920×1200 @ 60 Hz,
  encoder 507 and connector 508 (Linux-assigned IDs).
- **HDMI-A-1 uses DDI B / PHY B, CRTC pipe B**, 1920×1080 @
  **144 Hz**, encoder 516 and connector 517 (Linux-assigned IDs).
- HDMI mode on pipe B: pixel clock 346500 kHz, htotal 2080,
  hsync 1968..2000, vtotal 1157, vsync 1083..1088,
  active output 1920×1080 at 8 bits per component.
- Also advertised: 1920×1080 @ 60 Hz with 148500 kHz pixel clock;
  prefer this conservative mode for the first *future* hardware test.
- `i915_display_info` names DDI B/PHY B explicitly. The running pipe
  B assignment is an observed choice of the Linux DRM driver, **not**
  a permanent physical relationship that macOS should assume.

This confirms HDMI is physically driven by Intel i915, **not** a discrete
GPU, and establishes the HDMI **DDI B / PHY B** route. It does *not*
prove that macOS has powered the same PHY, that HDMI hotplug is active,
or which VBT, PLL, power-domain and PHY programming sequence is safe.
The Linux connector numbers are not hardware port IDs.

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

### Read-only macOS hardware-reference diagnostics

With this evidence, `NMIntelFB` publishes the hardware-specific
reference only when the PCI identity exactly matches Intel 8086:7d55,
Lenovo 17aa:3813. It snapshots the raw pipe B transcoder/primary plane
registers without claiming that HDMI is connected or switching modes:

```sh
ioreg -p IOService -l -w0 -c NMIntelFB | grep 'NMIntelFBHDMI'
```

The register snapshot is taken during framebuffer startup and is
**not a hotplug detector**. The Linux evidence is sufficient to avoid
guessing the physical DDI route; further Linux probe captures are not
needed for this mapping.

No new OpenCore connector patch is needed for this diagnostic.
Do not enable `i915_disable_display=0` or force `xegfx_no_display=0`
on the accelerated desktop: those bypass the eDP preservation guards.

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
