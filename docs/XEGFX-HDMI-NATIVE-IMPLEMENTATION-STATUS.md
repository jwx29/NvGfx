# XeGfx native Meteor Lake HDMI: implementation status

**Hardware:** Intel Meteor Lake 8086:7d55 / Lenovo 17aa:3813.
eDP A is the firmware-owned internal 1920x1200 panel.
HDMI B is the HP 25x external 1920x1080@60 sink.
**Branch:** `perf/xegfx-scanout-copy`.

## Hardware results verified

- DDI B GMBUS pin 2 returns a 256-byte EDID identical to Linux i915;
  checksum and preferred detailed timing are valid.
- DDC-based unplug/reconnect state changes are observed correctly.
- The eDP A frame counter advances on macOS; B port, transcoder,
  plane and surface are disabled/empty.
- Linux uses C10 DPLL1 (refclk 38.4 MHz, 4 lanes, 148.5 MHz),
  independently of eDP's DPLL0. DPLL1's raw C10 bytes are a
  **reference state, not a replayable program**.
- macOS `HSW_PWR_WELL_CTL2_DRV = 0xFC0F`. Linux identifies PW_2
  as index 1 and PW_B as index 6. Each request/state pair is set:
  `PW_2=0x000C`, `PW_B=0x3000`. This is evidence that those
  two power-well request and state bits are already asserted, **not**
  evidence that the DDI B IO/PHY, C10 PLL and DC-off wakerefs are
  owned/ready.

## Code completed in this stage

1. `hdmi_mtl_power.h`: exact MTL PW_2/PW_B request/state decoding;
   the root-only read-only modeset preflight reports
   `NMIntelFBHDMIPowerState` and `NMIntelFBHDMIPowerDriver`.
   It withholds the timing-only plan if required power-well bits
   are not asserted. It never flips another owner's request bits.
2. `hdmi_c10_msgbus.h`: reusable bounded MTL DDI B C10 PHY
   message-bus *transport*, with lane 0/1 register selection,
   committed/uncommitted writes, reads, ACK type/error checks,
   response W1C and timeouts. **Not called on hardware**:
   C10 operations require `DC_OFF`, PSR pause, an owned PHY, the
   correct power sequence and serialization. Do not invoke it by
   skipping those prerequisites.
3. `hdmi_native_sequence.h`: complete *transaction contract* with
   11 ordered stages and reverse-order rollback:
   acquire power, allocate independently pinned GGTT scanout,
   set up C10 DPLL, set up C10 PHY B, configure pipe B/DBUF,
   configure plane B, HDMI infoframes, **enable DDI B BEFORE
   TRANS_CONF_B** (MTL+), verify frame progression and publish
   head 5. The runner refuses to execute **any** stage unless the
   selected backend implements all 11 phases and their undo actions.
   It rechecks internal eDP and hardware ownership between phases.
4. `NMIntelFB::dryRunHDMINative` (admin property
   `XeGfxHDMINativeDryRun`): requires a current successful EDID
   and preflight; rechecks A frames, B ownership and PW_2/PW_B,
   then **reports the first missing backend** without writing any
   HDMI register. On this branch it returns `kIOReturnUnsupported`
   with `NMIntelFBHDMINativeStatus=native-backend-missing`.
5. `tools/hdmi-native-dry-run.c`: userland trigger to report the
   missing stage and number of applied hardware stages (zero).
6. Portable tests for C10 transport, MTL power masks and native
   transaction/rollback, added to `tools/test-hdmi-portable.sh`.

## What is NOT implemented yet

**An external picture will not appear from this stage.**
The sequential real-hardware backend is **not** implemented.
In particular, no code in this stage:
- takes a full refcounted DC-off/PSR and DDI B power/PHY ownership
  lock, or executes the full Intel C10 PHY and DPLL1 programming;
- allocates and pins its own independent *display scanout* GEM/GGTT
  object in XeGfx's i915 GPU memory manager, or makes it available
  to the macOS HDMI framebuffer without crossing kernel ownership;
- configures DBUF DDB allocation, watermarks, plane B and complete
  HDMI infoframe/clock path;
- creates an independent `IOFramebuffer` with live aperture, VBL
  and connection events or connects it to the staged NVAccel head 5;
- makes the HDMI port/transcoder enable writes.

These are *not* simple constant register writes. Display engine
initialization from Linux i915 is spread across the atomic modeset,
display power, DPLL, C10 PHY, DBUF and plane management paths.
A register replay with no rollback would risk a system hang or
corrupt the working internal display.

## Test

```sh
cd ~/src/NvGfx-merge-test
git pull --ff-only
bash tools/test-hdmi-portable.sh

# Rebuild and install via the usual XeGfx workflow; reboot.
xcrun clang -Wall -Wextra -Werror -framework IOKit \
  -framework CoreFoundation tools/hdmi-native-dry-run.c \
  -o hdmi-native-dry-run

sudo ./hdmi-edid-probe "$HOME/Desktop/hdmi-edid-macos.bin"
sudo ./hdmi-modeset-preflight
sudo ./hdmi-native-dry-run
```

**Expected for this stage:** `native-backend-missing` at
`power-wells`, zero stages applied. The power-well **status bits
can already be on** while no safe power-resource ownership/rollback
backend is implemented.

Do not boot with i915 native display KMS enabled; it is still
deliberately disabled to preserve the firmware eDP scanout.
Keep known-good EFI and kext rollback available.

Sources:
- Linux `intel_display_regs.h`, `intel_display_power_map.c`
  for PW_2 and PW_B bits;
- Linux `intel_cx0_phy_regs.h`, `intel_cx0_phy.c` for C10 message
  bus; these require DC_OFF and PSR quiescence;
- Aug/Oct 2026 MTL+ HDMI enable order in i915, DDI before
  transcoder;
- `docs/XEGFX-HDMI-LINUX-REFERENCE-20261029.md` for uploaded
  working Linux HDMI state.

## Next source milestone: i915 scanout lease + C10 image program (unproven on hardware)

On the current branch, `gpu/xkpi/src/i915/xg_i915_exec.c`
contains a real opt-in GEM system-memory allocation, pinned in the i915
GGTT, independent of the EFI pipe-A framebuffer. The XNU-facing
`xgHdmiSurface` platform-function bridge and the privileged
`XeGfxResident` diagnostic are implemented. See
[`XEGFX-HDMI-GGTT-SCANOUT.md`](XEGFX-HDMI-GGTT-SCANOUT.md).
Neither bridge nor allocator has been built or tested on the user's
macOS LinuxKPI source yet.

`kexts/NMIntelFB/hdmi_c10_pll_image.h` additionally implements the
Intel C10 **vendor-register programming order** for the
Linux-verified 1080p60 DPLL1 image. It snapshots the current C10 vendor
state (temporarily setting `MSGBUS_ACCESS`, as the Linux driver
requires), can restore it on partial program failures, and has a pure
mock test. **It is not called by the actual driver.** A real hardware
backend must first implement/exclusively hold DC_OFF, PSR disable,
PHY B ownership, PLL power request/ACK and lock, then program C10 PHY
and port B. The captured DPLL1 vendor data cannot be replayed safely
outside that sequence, nor can display become active without GGTT
scanout, DBUF/DDB and a second framebuffer.

Portable test runner:
`bash tools/test-hdmi-portable.sh`.

This remains a development milestone, **not functional HDMI output**.
