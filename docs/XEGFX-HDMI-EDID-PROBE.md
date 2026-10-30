# Experimental DDI B / GMBUS pin 2 EDID readback

**This does not enable HDMI display output.** This first milestone is a manually invoked, bounded
I2C/DDC diagnostic; no HDMI PHY, PLL, power wells, hotplug or scanout registers are written.

## Safety and scope

- The EDID read is **off by default** and only runs on a privileged IORegistry setProperties request.
- Exact PCI identity is required: Intel 8086:7d55, Lenovo 17aa:3813, with firmware eDP pipe A 1920x1200.
- The controller must be idle; inaccessible register values, busy/active bus and NAK are rejected.
- Only PCH GMBUS0..3 registers at 0xC5100..0xC510C are accessed; address 0x50, 100 kHz, pin 2.
- Only a base 128-byte EDID and its first 128-byte extension are read; both checksums are checked.
- Hardware writes are still required to operate GMBUS. Power-domain availability is **not** established.
- No fall back to GPIO bit-banging, no forced display power, and no default boot-time transaction.
- Before first macOS testing, preserve a working recovery EFI and known-good kernel extensions.

## Build and install

```sh
cd ~/src/NvGfx-merge-test
git pull --ff-only
bash ./build_xegfx_stack.sh --repo "$PWD" --out "$HOME/src/NvGfx-merge-build" --no-patches
export NVGFX_REPO="$HOME/src/NvGfx-merge-test"
export NVGFX_OUT="$HOME/src/NvGfx-merge-build"
bash ./install.sh --check
bash ./install.sh
```

Stop if the build/preflight fails; reboot after installing.

## Manually probe with HDMI connected (macOS)

```sh
cd ~/src/NvGfx-merge-test
xcrun clang -Wall -Wextra -Werror -framework IOKit -framework CoreFoundation \
  tools/hdmi-edid-probe.c -o hdmi-edid-probe
sudo ./hdmi-edid-probe "$HOME/Desktop/hdmi-edid-macos.bin"
ioreg -p IOService -l -w0 -c NMIntelFB | grep -E 'NMIntelFBHDMIEDID(Length|Status|GMBUS2)'
sudo log show --last 5m --style compact --predicate \
  'eventMessage CONTAINS "HDMI DDC/GMBUS2 one-shot probe"'
```

Expected HP 25x: status `ok`, 256 bytes, EDID manufacturer HPN, product 0x357f;
the first 128-byte block contains 1080p60 and the CTA extension describes 1080p144.
Compare the resulting binary with the known-good Linux `hdmi-monitor-edid.bin`.
Output created by sudo may be root-owned. Raw EDID is not logged or committed.

## GMBUS2.INUSE hardware semaphore (fixed after first macOS probe)

The first hardware probe returned `NMIntelFBHDMIEDIDStatus=busy`,
`NMIntelFBHDMIEDIDGMBUS2=0x8000`. The initial probe implementation
read `GMBUS2` twice during preflight. **Intel documents bit 15 as a
read-to-acquire software semaphore**, not simply a snapshot of another
driver using the bus: the first read returning bit 15 = 0 acquires the
semaphore; all subsequent reads report bit 15 = 1 until software
relinquishes it by writing `GMBUS2 = (1 << 15)`. The old code was
therefore able to acquire its own lock, misdiagnose the next read as
`busy`, and **return without releasing** that lock.

The corrected code reads `GMBUS2` once to acquire. If that first
read reports `INUSE=1`, it refuses to interfere with another owner
and does **not** release. If it acquired successfully, it releases
`INUSE` on **every** completion/error path, including a nonzero
GMBUS0 selection and active-controller refusal. Status polling after
acquisition must ignore the owner bit. The indexed EDID command also
uses `GMBUS_CYCLE_INDEX | GMBUS_CYCLE_WAIT`, matching Linux i915.

**Reboot before repeating the probe with the corrected kext.** A
previous invocation may have left its software semaphore claimed.
Do not write to GMBUS2 to force-clear unknown ownership while any
other driver could be using it; the fixed code intentionally refuses
a genuinely busy owner. If `busy` persists after reboot, inspect
the GMBUS0 selection and whether some other component controls DDC.

Intel reference: GMBUS2.INUSE read-to-acquire/one-to-clear,
<https://www.x.org/docs/intel/IVB/IHD_OS_Vol3_Part4.pdf>,
section 2.2.6. Linux source:
<https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/display/intel_gmbus.c.html>.

## Hardware verification — 29 October 2026

The user successfully ran the corrected probe under macOS Sequoia on the
Lenovo Intel 8086:7d55 / 17aa:3813 laptop with an HP 25x on HDMI. The
resulting `hdmi-edid-macos.bin` was compared byte-for-byte against the
previously exported Linux `hdmi-monitor-edid.bin`:

- Both files contain **256 bytes** and are **exactly identical (0 differing bytes)**.
- Base EDID and CTA extension checksums are both valid (sum mod 256 = 0).
- Both identify manufacturer **HPN**, product **0x357f** and a
  1080p60 preferred mode with CTA 1080p144 timing.
- This validates the macOS DDI B / GMBUS pin 2 hardware DDC path and the
  corrected GMBUS semaphore acquisition/release implementation.
- The earlier failure (`busy`, `GMBUS2=0x8000`) was caused by a
  double-read of the read-to-acquire `GMBUS2.INUSE` semaphore and
  was corrected before the successful EDID capture.

**What remains unverified:** live HPD interrupt/plug detection, DDI B
PHY/PLL and HDMI TMDS output configuration, pipe B scanout ownership,
a separate macOS `IOFramebuffer` head, and display sleep/wake. Do not
conflate successful DDC readback with HDMI video output. Preserve eDP
pipe A throughout future bring-up.

The monitor's raw EDID and serial number are **not** committed.

## Failure codes

- `busy`: GMBUS0 nonzero or controller INUSE/ACTIVE; refuse to seize another user's bus.
- `inaccessible`: inaccessible (all-ones) MMIO reading; power well may be off.
- `nak`: no EDID acknowledgement, possibly an unpowered HDMI DDC path.
- `timeout` / `idle-timeout`: bounded controller transaction could not finish.
- `bad-header` / `bad-checksum`: readback invalid, not accepted as an EDID.

Do not bypass XeGfx's i915 display-disable guards to address these failures.

## Offline portable tests

```sh
c++ -std=c++17 -Wall -Wextra -Werror -I kexts/NMIntelFB \
  tools/test_hdmi_gmbus.cpp -o /tmp/test_hdmi_gmbus
/tmp/test_hdmi_gmbus
```

These exercise the state machine with simulated MMIO; they do not establish whether
the Meteor Lake GMBUS power domain is accessible on macOS.

References: Linux i915 `intel_gmbus.c` and `intel_gmbus_regs.h` (MIT).
See `docs/XEGFX-HDMI-TOPOLOGY.md` for the verified Lenovo VBT and monitor EDID.
