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

### Confirmed HDMI DDC/GMBUS routing (Linux, 2026-10-29)

```text
/sys/class/drm/card1-HDMI-A-1/ddc
  -> /sys/devices/pci0000:00/0000:00:02.0/i2c-3
/sys/class/drm/card1-HDMI-A-1/ddc/name
  -> i915 gmbus dpb
```

For the **Meteor Lake** PCH, the i915 `gmbus_pins_mtp` table maps
**`dpb` to `GMBUS_PIN_2`** (hardware GMBUS selection **2**,
GPIO_2). The `i2c-3` Linux adapter index is merely an assigned I2C
adapter number, not the GMBUS pin ID.

Older WhateverGreen Apple framebuffer documentation identifies
`busId=5` with the *older* GMBUS_PIN_DPB convention; that is **not**
evidence that 5 is correct for the new Meteor Lake/GMBUS implementation.
WhateverGreen framebuffer patch properties apply to Apple's framebuffer
tables and are not consumed by the custom `NMIntelFB` driver.

Relevant upstream code:
https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/display/intel_gmbus.c

### Board firmware VBT and ACPI OpRegion (2026-10-29)

The debugfs files are in fact present on Nobara Linux under
`/sys/kernel/debug/dri/0000:00:02.0/`; earlier globbing missed them.
The user extracted both and decoded the VBT with
`intel_vbt_decode --file=hdmi-vbt.bin --devid=0x7d55`.

**VBT**: `$VBT METEORLAKE`, header size 48 bytes, declared VBT length
7249 bytes (Linux debugfs export 7680 bytes including padding),
BDB version 256 and child-entry size 40 bytes. BDB block 2 is the
firmware General Definitions table. HDMI is child **EFP1**:

| Decoded VBT field | Value | Meaning for XeGfx |
| --- | --- | --- |
| Device handle | `0x0004` / EFP1 | Firmware external-output entry |
| Device type | `0x60D2` (decoder: DVI-D) | Same value as Linux `DEVICE_TYPE_HDMI`; TMDS-capable |
| DVO port | `DP-B (0x07)` | Intel **DDI B/PHY B**, confirmed by Linux i915 |
| DDC pin | `0x02` | Confirms DPB's actual **GMBUS pin 2** |
| AUX channel | `none (0x00)` | This output is not an AUX-driven DP link |
| Hotplug signaling | advertised | Capability; live HPD detection still needs implementation |
| Onboard LSPCON | no | No firmware-declared LSPCON bridge on EFP1 |
| HDMI max data rate | platform max (`0x00`) | No explicit per-output pixel-clock cap in this field |
| HDMI Max FRL rate | FRL not supported | Use conventional TMDS, not HDMI 2.1 FRL |
| HDMI level shifter | `0x00` | Use Intel/board programming, do not guess a WEG BusID |

Note: the decoder labels `0x60D2` as DVI-D for legacy reasons;
Linux's `intel_vbt_defs.h` also defines `DEVICE_TYPE_HDMI 0x60D2`.
Do **not** reinterpret the entry as a DisplayPort/AUX connector
because its DVO port is named DP-B. In the running kernel, it is
`HDMI-A-1` driven by DDI B and communicating over `i915 gmbus dpb`.

The firmware describes internal **LFP1** as an eDP/DP-A output:
device handle `0x0008`, DVO port `DP-A (0x0a)`, AUX-A `0x40`.
Other external child entries are DisplayPort/USB-C oriented and do
not replace the confirmed EFP1 HDMI route.

The `intel_vbt_decode` warnings about the panel type/EDID and
block minimum sizes do not change the EFP1 routing fields.
The separate 8192-byte `i915_opregion` dump is Intel OpRegion
version 3. Its conventional embedded VBT area (`0x400..0x1bff`)
is empty; the debugfs VBT export is therefore the relevant source.
The OpRegion contains a valid panel EDID near `0x1c04`, identifying
the laptop panel as **LEN140WUXGA**; that is **not** the HDMI EDID.

Raw firmware dumps were **not** committed to the repository.
Keep the original binaries local and avoid assuming a VBT will be
located inside every macOS OpRegion mapping.

### Consequences for HDMI support

No HDMI BusID remap is indicated by the firmware: EFP1 is already
DDI B with DDC pin 2, matching Linux. WhateverGreen's connector
`busid` override patches Apple's framebuffer structures, but
`NMIntelFB` is its own framebuffer and does not consume those keys.

The next meaningful development stage is safe **EDID readback on
GMBUS pin 2** (with appropriate display power-domain/HPD handling),
then a DDI B HDMI TMDS PHY/PLL/link and independent scanout
initialisation. Do not enable pipe B by toggling register bit 31
without those steps; preserve firmware-controlled eDP pipe A.

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
2. Use the confirmed VBT EFP1/DP-B/DDC pin 2 information to
   validate safe HDMI HPD and read EDID over GMBUS with appropriate
   power-domain handling; do not reprogram pipe A.
3. Make exclusive ownership of the *new* display pipe and transcoder
   while preserving firmware eDP A, including CDCLK/shared DPLL
   allocation, power wells, DDI PHY, audio/infoframes and hotplug.
4. Add a second independent IOFramebuffer with its own scanout
   allocation, timing, EDID, connect interrupt, VBL, lifecycle and
   matching NVAccel/AGDC head routing.
5. Verify plug/unplug, safe rollback, standby/wake and WindowServer
   restart before making the native HDMI path default.

No active HDMI modesetting is implemented or enabled by this diagnostic commit.
