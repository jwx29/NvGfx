# XeGfx

Work in progress: Intel Xe (Gen12+) graphics for macOS, built from FreeBSD's drm-kmod (Linux i915, MIT) on a
LinuxKPI-for-XNU layer. Nothing here loads yet.

- `harness/score.py`: compiles every drm, dma-buf, ttm and i915 source for the XNU kernel target and ranks what
  breaks. Sources are expected in `~/src/xe-src` (linux, freebsd, drm-kmod).
- `shim/include`: XNU overrides of FreeBSD and LinuxKPI headers. `shim/gen`: generated FreeBSD interface headers
  and empty kernel option headers.
