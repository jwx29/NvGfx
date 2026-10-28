# XeGfx scanout copy performance A/B test

This experimental patch is for **XeGfxAccel on Intel only**; other GPU accelerators retain the original code.
It does **not** implement zero-copy display scanout. That would require a validated Intel i915 display flip
path, not an unverified physical-address substitution.

## Why window dragging can be slow

Two paths copy whole composited frames from WindowServer surfaces to the firmware-programmed
Intel panel. The old code performs one **volatile 32-bit read and write per pixel** on the calling
thread. It can be especially slow when the source is GPU-backed or the destination is write-combined
memory. A fresh upstream XeGfx kernel alone cannot eliminate that copy.

The first change is a shared row-copy helper with an opt-in `memcpy` implementation.
`memcpy` can generate wider writes and reduce loop overhead on ordinary memory mappings,
but is **not** guaranteed to beat the original loop or work on every cache mapping.

## Test

After building and installing the new XeGfxAccel and rebooting, leave your original mode enabled:

```sh
sysctl debug.nvaccel_scanout_bulk_copy # 0: original loop
sysctl debug.nvaccel_scanout_last_us debug.nvaccel_scanout_copies
sysctl debug.nvaccel_iop_copy_frames debug.nvaccel_iop_copy_last_us debug.nvaccel_iop_copy_sum_us
```

Drag the same windows for 15–30 seconds. Record counter deltas, not only the latest value.
Legacy swap-path copies are measured by `nvaccel_scanout_copies` and
`nvaccel_scanout_sum_us`; display-transaction copies by `nvaccel_iop_copy_frames` and
`nvaccel_iop_copy_sum_us`. Average copy time is the change in sum divided by the change
in count (microseconds/frame). A zero change in both counts suggests that the bottleneck
is elsewhere; don't infer any benefit from this patch.

Then, without restarting WindowServer:

```sh
sudo sysctl -w debug.nvaccel_scanout_bulk_copy=1
```

Repeat the **same** drag test and compare average copy time and perceived responsiveness.
To return to the previously working scalar path immediately:

```sh
sudo sysctl -w debug.nvaccel_scanout_bulk_copy=0
```

This is a runtime-only sysctl, defaults to zero after every reboot, and is Xe-only.
If bulk copying introduces any corruption, flicker, freeze or higher latency, disable it.
Do **not** add this to boot-args or startup automation until target-hardware testing shows
it is beneficial and stable. Framebuffer writes still take place on the original CPU
presentation path; a larger improvement requires GPU-side scanout/flipping.

## Unit tests

```sh
c++ -std=c++17 -Wall -Wextra -Werror tools/test_xegfx_scanout_copy.cpp -o /tmp/test_xegfx_scanout_copy
/tmp/test_xegfx_scanout_copy
```

The host test checks scalar and bulk output equivalence, row padding and same-buffer
no-op semantics. It cannot reproduce actual GPU memory mapping/cache behaviour.
