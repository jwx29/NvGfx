# XeGfx Meteor Lake scanout optimisation

This update enables the Xe-only bulk scanout row path by default and implements
it with x86 `REP MOVSB` + a trailing `SFENCE`, following the integer-only
transfer strategy in VedCloud's `latitude_front_copy.h`. The original
volatile 32-bit scalar copy remains available at runtime:

```sh
sudo sysctl -w debug.nvaccel_scanout_bulk_copy=0 # revert
sudo sysctl -w debug.nvaccel_scanout_bulk_copy=1 # fast
```

The change applies to both Xe display transaction and legacy swap scanout
copies and retains row padding, framebuffer self-copy checks, timing
counters and the existing memory mapping. Other NMX/NVIDIA accelerators
do not use this helper. It does not remove Vulkan image readback or implement
native Intel display-plane flipping.

**Caution:** this alters a kernel framebuffer-memory hot path. Build and
verify the kext on the target macOS SDK, keep a rollback EFI, and revert if
you see stalls, corruption, or regressions. Faster copying is not a
guaranteed higher window-drag frame rate.

Reference: https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics/blob/main/NullMoth/kexts/NVRM/accel/nmx/latitude_front_copy.h
