# Native HDMI enablement on Meteor Lake

The current NMIntelFB driver publishes one firmware-lit screen and cannot
initialize a second HDMI connector after boot. Laptop firmware that only
initializes the internal eDP panel cannot use the firmware-primary HDMI trial.

**Do not set** `compat.linuxkpi.i915_disable_display=0` or
`compat.linuxkpi.i915_xegfx_no_display=0` on a production boot. XeGfx
deliberately reserves the firmware console's stolen-memory allocation and
checks that i915 has left its scanout alone. i915's native KMS must first
be isolated and integrated with NMIntelFB, NVAccel and AGDC.

To find the actual DDI/port mapping, boot Linux with HDMI plugged in, then:

```bash
sudo mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
bash tools/hdmi-linux-probe.sh | tee hdmi-linux.txt
```

Optionally re-run with `sudo bash tools/hdmi-linux-probe.sh` to collect
i915 display debugfs details (but don't run elevated if not needed).
Run once with HDMI connected and, if necessary, once disconnected; send
`hdmi-linux.txt` for port-specific integration.

Native implementation milestones:
- Identify physical HDMI connector, DDI/HPD pin and whether output uses
  native HDMI TMDS, a DP-to-HDMI protocol converter or USB-C alt-mode.
- Use/port i915 connector handling, DDC/EDID, encoder/PLL/PHY modesetting,
  hotplug and modeset lifecycle without resetting firmware eDP pipe A.
- Provide second validated scanout aperture/framebuffer instance, independent
  power/hotplug/VBL, IOFBDependentIndex and AGDC/accelerator per-head routing.
- Prove rollback from unplug, mode changes, sleep/resume and WindowServer reset.

Linux i915 hotplug design: https://origin.kernel.org/doc/html/latest/gpu/intel-display/hotplug.html
XeGfx internal architecture: gpu/xegfx/docs/HOW-IT-WORKS.md.
