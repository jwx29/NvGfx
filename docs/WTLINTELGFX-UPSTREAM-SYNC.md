# WTLIntelGfx upstream integration — 2026-10-31

Source: https://github.com/watchthelight-labs/WTLIntelGfx
Pinned upstream commit: `ff64f8361a11a0995dae2cf5c4528752f388d5bb` (XeGfx 0.0.47).

This is a selective, file-level upstream merge into the established NvGfx
super-repository structure, **not** a Git history merge. The two repositories
have different root layouts. Core source under upstream `shim/`, `kext/`,
`harness/` and `tools/` maps to `gpu/xegfx/` here.

Merged:
- XeGfx core v0.0.47 fixes (including 0380 display-other iGPU and
  null-format logging), bundle metadata, Intel documentation.
- Upstream Metal/ANV changes as context-matched patch hunks. Fork-specific
  WindowServer host-copy, VRAM and presentation changes were kept.
- Separate latest XeGfxAccel and XeGfxAGDC sources imported alongside
  the existing NVRM-based production code, without changing default builds.
- XeGfxIntelFB 0.9 and Intel display planning utilities.
- Optional `build/nvgfx/build_xegfxaccel.sh` builds upstream Intel-only
  XeGfxAccel from this super-repo (defaults to macOS 15;
  `XEGFX_ACCEL_MAJOR=26` for Tahoe). The existing tested
  `build_xegfx_stack.sh` is unchanged.

Kept unchanged:
- The entire `kexts/NMIntelFB/` Meteor Lake HDMI extension and its tests;
  upstream NMIntelFB lacks these changes.
- `gpu/xkpi/` GGTT HDMI lease implementation.
- Fork-patched harness, linker, original XeGfxAccel build and install scripts.
- `gpu/userland/` path overrides and proprietary NVIDIA-stack sources.
- Existing OpenCore/boot configuration.

**Safety:** the upstream XeGfxIntelFB 0.9 hardware cursor and the alternative
XeGfxAccel have not been validated on Meteor Lake/Sequoia.
Do not auto-install or enable either one. The original boot-path build scripts
and known-good installation remain available.

Validation on macOS:
```sh
bash tools/test-hdmi-portable.sh
bash build_xegfx_stack.sh
# Optional, compile-only, not used by default:
# XEGFX_ACCEL_MAJOR=15 bash build/nvgfx/build_xegfxaccel.sh
```
