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
