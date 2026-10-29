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

## EDID-derived timing descriptor (staged, not applied)

A portable, checksum-validated parser in `kexts/NMIntelFB/hdmi_edid_timing.h`
reads the **first detailed timing descriptor** from EDID 1.3/1.4,
accepting only digital separate-sync timings with internally consistent
pixel clock, active size, blanking and sync windows. The one-shot HDMI
probe publishes `NMIntelFBHDMIPreferredTiming` (10 little-endian
`uint32_t` values, 40 bytes) only when a validated EDID yields such a
mode, and removes it before every new DDC read. Layout:

```text
pixelClockHz,
hActive, hSyncStart, hSyncEnd, hTotal,
vActive, vSyncStart, vSyncEnd, vTotal,
edidSyncFlags
```

For the validated HP 25x EDID, expect:
`148500000, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 0x1e`.
This is **not a modeset**; no registers are written by the parser.

Offline parser test (may also pass the known-good EDID):

```sh
c++ -std=c++17 -Wall -Wextra -Werror -I kexts/NMIntelFB \\
  tools/test_hdmi_edid_timing.cpp -o /tmp/test_hdmi_edid_timing
/tmp/test_hdmi_edid_timing
/tmp/test_hdmi_edid_timing /path/to/hdmi-monitor-edid.bin
```

## HDMI stage 3: verified hardware modeset preflight (read-only)

The macOS HDMI DDC/EDID reader was tested on hardware and detected
connected -> unresponsive -> connected. To prepare **real** DDI B TMDS
video bring-up, this stage adds a privileged **read-only** modeset
preflight, without enabling DDI B, allocating a second GGTT scanout,
or registering a fake monitor with WindowServer.

On the exact Lenovo Intel 8086:7d55 / 17aa:3813 system, the preflight
requires that the previous GMBUS read returned a checksum-valid
**1920x1080@60 Hz, 148500000 Hz** preferred timing. It verifies:
- firmware eDP pipe A is still the selected 1920x1200 panel;
- the eDP A plane and transcoder are still enabled and FRMCOUNT advances;
- pipe/transcoder/plane B and DDI B port enable bits are **not** already
  active or owned by another display path;
- all sampled power-well, clock, transcoder and plane register snapshots
  are accessible, without consulting sideband PHY registers or reading
  the side-effectful GMBUS2 semaphore.

It publishes:
- `NMIntelFBHDMIPreflightStatus`: explicitly **not**
  a claim that HDMI modesetting is fully implemented;
- `NMIntelFBHDMIPreflightPanelFrames` (40ms internal-panel frame delta);
- `NMIntelFBHDMIPreflightTargetClockHz`;
- `NMIntelFBHDMIPreflightRegisters`: a binary dump with
  `uint32 abi=0x48445031, uint32 count=22`, followed by
  22 offsets and 22 values, all little-endian 32-bit words.

### Running the preflight under macOS

After installing the new NMIntelFB and rebooting, with HDMI connected:

```sh
cd ~/src/NvGfx-merge-test
xcrun clang -std=c11 -Wall -Wextra -Werror \
  -framework IOKit -framework CoreFoundation \
  tools/hdmi-modeset-preflight.c -o hdmi-modeset-preflight

# First update validated GMBUS/EDID data; writes only the GMBUS controller:
sudo ./hdmi-edid-probe "$HOME/Desktop/hdmi-edid-macos.bin"

# Then capture power/clock/port/pipe registers without hardware writes:
sudo ./hdmi-modeset-preflight | tee "$HOME/Desktop/hdmi-macos-preflight.txt"
```

The preflight never turns on HDMI, even if the status is
`ready-for-modeset-implementation`. That status means only that
a validated monitor mode, moving eDP A and unoccupied pipe B were seen.

### Compare Linux's working HDMI mode

Boot Linux with the HDMI monitor actually scanning out **1080p60**
(not just plugged in). If `intel_reg` is available, run:

```sh
bash tools/hdmi-linux-register-reference.sh --i-understand-mmio-risk \
  | tee ~/hdmi-linux-modeset-registers.txt
```

This samples the **same** 22 register offsets on the *same* Intel PCI
GPU using `intel_reg --pci-slot=0000:00:02.0 read ...`. It does not
write registers, but any low-level MMIO reads can still hang hardware.
The DDI_B port and pipe enables, clock selection, power-well state and
transcoder timing should differ between working Linux HDMI and macOS
firmware-only eDP. Some values also vary with the selected Linux mode.
**Do not paste Linux register values directly into macOS display MMIO.**
Firmware display states, power-well ownership and DPLL/CDCLK allocation
are different, so a naive register replay risks a panic or black screen.

Relevant Linux register definitions:
https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/display/intel_display_regs.h.html.
MTL+ ordering: Intel's August/October 2026 proposed i915 changes
enable `DDI_CTL_DE` before `TRANS_CONF` for HDMI TMDS.
Linux must also program C10/C20 PHY link, pipe configuration, shared
PLL, power wells and GGTT scanout independently.

### Next code change required for visible HDMI output

The preflight data is sufficient to stop guessing the port/pipe register
states. To actually light the monitor, a new coordinated display-engine
controller must:

1. Obtain display power/PHY/PLL ownership without resetting eDP A,
   and verify DDI B PHY and display power wells are ready.
2. Allocate GGTT pinned 1920x1080 linear XRGB scanout distinct from
   firmware eDP memory, respecting MTL stolen-memory workarounds.
3. Configure HDMI sink, DDI B clock/PHY/TMDS, transcoder B and pipe B
   in the **MTL+** order, with timeouts and rollback of *owned* state.
4. Publish a second real framebuffer with independent VBL, EDID,
   power/connection callbacks and `IOFBDependentIndex=5` /
   `XeGfxNativeHDMIFramebuffer=true`, and hook XeGfxAccel head 5
   to its separate mapped scanout.
5. Implement unplug, sleep/wake and WindowServer recovery; never make
   experimental hardware modesets automatic at boot.

None of these display writes is enabled by the stage-3 preflight.

Run all portable HDMI tests before rebuilding kernel components:

```sh
bash tools/test-hdmi-portable.sh
# Optional: also validate the preferred DTD against the original monitor EDID:
HDMI_MONITOR_EDID=/path/to/hdmi-monitor-edid.bin bash tools/test-hdmi-portable.sh
```

The macOS kernel kext has **not been built on the developer's target
machine by this commit**. Passing portable unit tests does not prove
the Intel power/PHY/display programming will work; that part is still
unimplemented.
