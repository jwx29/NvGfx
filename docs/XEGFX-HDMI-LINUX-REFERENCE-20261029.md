# Meteor Lake HDMI B: verified Linux 1080p60 reference

**Source:** user-uploaded working Nobara i915 debugfs captures,
`hdmi-linux-display-info.txt`, `hdmi-linux-plls.txt`,
`hdmi-linux-power.txt` (29 Oct 2026). Monitor: HP 25x. Platform:
Intel Meteor Lake iGPU PCI 8086:7d55 / Lenovo subsystem 17aa:3813.

**Never treat these as a register-replay or display-enable sequence.**
Linux debugfs reports *state after modesetting*. Display power ownership,
C10 PHY/PLL programming, CDCLK, DBUF, GGTT, framebuffers, HDMI infoframes,
pipe clocks and rollback must be coordinated to reproduce this state.

## Active display topology

| Linux i915 CRTC | eDP internal | HDMI external |
| --- | --- | --- |
| Pipe | A | B |
| Output | DDI A / PHY A / eDP-1 | DDI B / PHY B / HDMI-A-1 |
| Mode | 1920x1200 at 60 Hz | **1920x1080 at 60 Hz** |
| Pixel clock kHz | 154250 (adjusted) | **148500** |
| Port clock kHz | 324000 | **148500** |
| Link lanes | 2 | **4** |
| Pipe bits per pixel | 30 | **24** |
| CRTC active | yes | yes |
| Primary plane | plane 1A | plane 1B |
| Framebuffer format | XR30 | XR30 |

**Important:** Linux's plane framebuffer pixel format **XR30** (10-bit RGB
storage) is not the same as the HDMI pipe's transmitted `bpp=24`
and sink's `max bpc=8`. Do not copy the Linux plane format blindly
into NMIntelFB's 32-bit firmware XRGB scanout pipeline. Separate
format conversion, buffer allocation and DBUF programming remain.

The active HDMI 1080p60 detailed timing is:

```text
clock 148500 kHz
horizontal: active=1920, sync_start=2008, sync_end=2052, total=2200
vertical:   active=1080, sync_start=1084, sync_end=1089, total=1125
```

The frame/refresh calculation gives exactly 60 Hz:
148500000 / (2200 * 1125) = 60.

## Dedicated C10 DPLL1 HDMI clock (active under Linux)

The i915 `i915_shared_dplls_info` output reports 38.4 MHz
non-SSC reference clock. The two active PLLs are **independent**:

- **DPLL0**, pipe_mask/active `0x1`: internal eDP pipe A, C10,
  2 lanes, multiplier **168**, tx_clk_div **1**, fracn quot **49152**.
  Leave this PLL and its firmware display path alone on macOS.
- **DPLL1**, pipe_mask/active `0x2`: external HDMI pipe B, C10,
  **4 lanes**, multiplier **77**, tx_clk_div **2**, fracn quot
  **22528**, remainder **0**, denominator **1**,
  SSC **off**, refclk **38400 kHz**.

DPLL1's Linux C10 raw state (reference data **only**, not programmed
to macOS):

```text
c10pll_rawhw_state: tx=0x10, cmn=0x01
pll[0]=f4 pll[1]=00 pll[2]=7a pll[3]=00
pll[4]=00 pll[5]=00 pll[6]=00 pll[7]=00
pll[8]=00 pll[9]=01 pll[10]=00 pll[11]=00
pll[12]=58 pll[13]=00 pll[14]=00 pll[15]=0a
pll[16]=06 pll[17]=0f pll[18]=85 pll[19]=23
```

These raw bytes belong to the selected PHY/PLL instance and require
version-matched Intel C10 sideband transactions, proper power
dependencies, lock polling, and ownership tracking. On macOS DPLL1
must be checked for availability: its Linux ID is **not** proof that
it is available in another boot state. Intel i915 implements C10
HDMI PLL calculation in `intel_snps_hdmi_pll.c`.

## Working Linux power-domain dependencies

The `i915_power_domain_info` capture shows:

```text
always-on: enabled
  DISPLAY_CORE         use=2
  PIPE_A               use=1
  PIPE_B               use=1
  TRANSCODER_A         use=1
  TRANSCODER_B         use=1
  PORT_DDI_LANES_A     use=1
  PORT_DDI_LANES_B     use=1
  PORT_DDI_IO_B        use=1
  AUDIO_MMIO           use=1
  AUX_IO_A             use=1
  DC_OFF               use=1
PW_1                   use=0
DC_off                 use=2
PW_2                   use=2
  PIPE_B               use=1
  TRANSCODER_B         use=1
PW_A                   use=1
PW_B                   use=2
  PIPE_B               use=1
  TRANSCODER_B         use=1
GMBUS                  use=0 at capture instant
```

MTL uses Xe-LPD+ power mapping. `PORT_DDI_IO_B` is managed in the
**PLL** logic, not by simply setting legacy DDI IO power-well
registers. PW_2 / PW_B are implicated by Linux's active pipe B and
transcoder B. Proper acquire/release order is necessary.
The macOS preflight's `0x45450=0` and `0x45454=0` alone do
not establish MTL DDI B power state.

Reference:
<https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/display/intel_display_power_map.c.html>.

## Compared with macOS preflight

Successful macOS EDID: 256 bytes, HP 25x, valid checksum, preferred
1080p60. MMIO after EFI adopted the internal panel:

```text
TRANSCONF_A       0x70008 = c0000000  eDP A active
PLANE_CTL_A       0x70180 = 84000008  eDP plane active
DDI_CTL_DE_A      0x64000 = 80000002  eDP port active
TRANSCONF_B       0x71008 = 00000000  HDMI transcoder off
PLANE_CTL_B       0x71180 = 00000008  HDMI plane off
PLANE_SURF_B      0x7119c = 00000000  no HDMI scanout
DDI_CTL_DE_B      0x64100 = 00000080  HDMI port disabled
DDI_CLK_SEL_B     0x46104 = 00000000  selection absent
TRANS_CLK_SEL_B   0x46144 = 00000000  selection absent
HTOTAL_B          0x61000 = 00000000  no timing
VTOTAL_B          0x6100c = 00000000  no timing
```

The internal eDP A frame counter progressed by 3 frames in 40 ms.

## Newly implemented timing-only plan

`kexts/NMIntelFB/hdmi_mode_plan.h` implements
`buildModePlan()`. A *validated* macOS preflight publishes its
`NMIntelFBHDMIModePlan` **only** if an EDID matches the
working Linux 1080p60 mode and eDP A is live while DDI B / pipe B
are disabled. Binary ABI `0x484d5031` ('HMP1'), 96 bytes.

The plan encodes the following **expected but NOT WRITTEN** timing
values (Linux i915 sets `TRANS_VBLANK_START=1` on ADL+):

| Register | Offset | Computed value |
| --- | --- | --- |
| TRANS_HTOTAL_B | 0x61000 | `0x0897077f` |
| TRANS_HBLANK_B | 0x61004 | `0x0897077f` |
| TRANS_HSYNC_B | 0x61008 | `0x080307d7` |
| TRANS_VTOTAL_B | 0x6100c | `0x04640437` |
| TRANS_VBLANK_B | 0x61010 | `0x04640000` |
| TRANS_VSYNC_B | 0x61014 | `0x0440043b` |
| PIPEBSRC | 0x6101c | `0x077f0437` |

**Additional timing required:** `TRANS_SET_CONTEXT_LATENCY`
used for VBLANK on display version >=13 (as in modern i915);
`TRANS_VSYNCSHIFT`; pixel format / pipe bpp / HDMI infoframes.
Do not enable the transcoder with only these values present.

## Native HDMI implementation gates still missing

1. Native MTL display-power domain manager with reference-counted
   acquire/release (without touching the firmware eDP ownership).
2. C10 DPLL1 clock calculation/programming, MTL PHY sideband access,
   HDMI TMDS transmitter setup, and lock/error rollback.
3. GPU-resident independent linear scanout in properly pinned GGTT,
   not BAR2+PLANE_SURF guessed aperture; DBUF, watermarks, plane format.
4. Separate `IOFramebuffer` for HDMI and XeGfxAccel head 5,
   connecting WindowServer to **its** scanout buffer.
5. Correct link/transcoder enable order, audio/infoframes, connection
   interrupts, unplug, sleep/wake and failure recovery.

None of these is activated by the timing plan or preflight. A passing
preflight is **not** permission to set `TRANSCONF_B.ENABLE`.

Linux references:
- <https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/display/intel_display_regs.h.html>
- <https://android.git.googlesource.com/kernel/common/+/cf99b26d3081b1f9961cc213415867706d19a414/drivers/gpu/drm/i915/display/intel_display.c>
- <https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/display/intel_display_power_map.c.html>
- <https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/display/intel_snps_hdmi_pll.c.html>
