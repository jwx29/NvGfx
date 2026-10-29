# Native HDMI enablement on Meteor Lake

The current NMIntelFB driver publishes one firmware-lit screen and cannot
initialize a second HDMI connector after boot. Laptop firmware that only
initializes the internal eDP panel cannot use the firmware-primary HDMI trial.

**Do not set** `compat.linuxkpi.i915_disable_display=0` or
`compat.linuxkpi.i915_xegfx_no_display=0` on a production boot. XeGfx
deliberately reserves the firmware console's stolen-memory allocation and
checks that i915 has left its scanout alone. i915's native KMS must first
be isolated and integrated with NMIntelFB, NVAccel and AGDC.

The Linux i915 capture now confirms a physical **DDI B / PHY B** HDMI
connector (HDMI-A-1), using **pipe B** in that particular session at
1920×1080/144 Hz. Internal eDP is on DDI A/PHY A and pipe A. The same
HDMI monitor advertises 1920×1080/60 Hz, a better initial test mode.
See `docs/XEGFX-HDMI-TOPOLOGY.md`.

For additional kernel display diagnostics, Linux can still report:

```bash
sudo mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
bash tools/hdmi-linux-probe.sh | tee hdmi-linux.txt
```

Optionally re-run with `sudo bash tools/hdmi-linux-probe.sh` to collect
i915 display debugfs details (but don't run elevated if not needed).
The DDI port mapping has already been established. Native HDMI output is
still unavailable on macOS until the display-engine ownership, DDC/EDID,
PLL/PHY programming and independent framebuffer/accelerator path exist.

Native implementation milestones:
- Confirm HDMI HPD/DDC and transmitter configuration on DDI B; port B is
  identified, but its safe power-domain/PHY sequence still needs porting.
- Use/port i915 connector handling, DDC/EDID, encoder/PLL/PHY modesetting,
  hotplug and modeset lifecycle without resetting firmware eDP pipe A.
- Provide second validated scanout aperture/framebuffer instance, independent
  power/hotplug/VBL, IOFBDependentIndex and AGDC/accelerator per-head routing.
- Prove rollback from unplug, mode changes, sleep/resume and WindowServer reset.

Linux i915 hotplug design: https://origin.kernel.org/doc/html/latest/gpu/intel-display/hotplug.html
XeGfx internal architecture: gpu/xegfx/docs/HOW-IT-WORKS.md.

## HDMI bring-up stage 2: DDC state and second-head routing (opt-in)

The macOS DDI B GMBUS pin 2 path has produced an **exact 256-byte match**
against the Linux EDID of the HP 25x monitor. That proves DDC, not HDMI
video. Two follow-on, safely dormant infrastructure changes are now present:

- NMIntelFB records every manually requested DDC probe as
  `NMIntelFBHDMIDDCState = connected | unresponsive | unknown`,
  plus `NMIntelFBHDMIDDCGeneration` and `NMIntelFBHDMIDDCProbeCount`.
  `connected` means valid EDID; `unresponsive` means GMBUS NAK;
  power-gated, busy and checksum errors are `unknown`, **not** unplug proof.
  No IOFramebuffer connect notification is issued on this single eDP FB.
- NVAccel reserves **index 5** for a future HDMI native framebuffer
  while preserving the existing **index 4** eDP path. Head 5 is selected
  only when a genuinely separate IOFramebuffer publishes both
  `XeGfxNativeHDMIFramebuffer = true` and `IOFBDependentIndex = 5`.
  The new head is NOT created or registered by this stage and cannot
  produce HDMI video on its own.

To poll explicit DDC connectivity on macOS (no boot-time polling):

```sh
cd ~/src/NvGfx-merge-test
xcrun clang -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation \
  tools/hdmi-edid-probe.c -o hdmi-edid-probe
sudo ./hdmi-edid-probe --watch 5
```

Stop with Ctrl-C. Connect/disconnect an HDMI monitor to observe the DDC
state and generation. Keep the poll interval >= 2 s. Polling is diagnostic
and should not be left running unnecessarily; monitor power-save behavior
can cause occasional NAK/unknown results even with a physically plugged cable.

Remaining critical HDMI video work:

1. Implement proper, coordinated Intel MTL display-power wells, DDI B
   clock/PHY and TMDS transmitter configuration using the version-matched
   Linux i915 sequences, preserving firmware eDP A, its PLL and CDCLK.
2. Allocate and pin a second genuine GPU-scanout surface in GGTT and
   initialise free pipe/transcoder/plane with EDID-validated 1080p60 timings.
3. Create a separately owned IOFramebuffer using the explicit head-5
   marker, independent VBL, lifecycle and NVAccel copy destination.
4. Add real HPD IRQ handling, encoder shutdown, resume and rollback.

Do NOT toggle TRANS_CONF_B bit31 as a shortcut. In particular, recent
i915 MTL+ HDMI enable ordering configures/enables the port before
the transcoder; a partial register sequence risks hanging the desktop.
Source: https://www.mail-archive.com/intel-gfx@lists.freedesktop.org/msg385935.html
