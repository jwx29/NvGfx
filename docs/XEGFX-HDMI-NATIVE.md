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
