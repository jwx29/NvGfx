# Intel Xe on the NvGfx stack (branch `xe`)

> **Heads up:** a good portion of the changes in this fork are AI generated. I do understand how they work,
> and I'm happy to explain any of it. If something looks wrong or you have a question,
> open an issue or ping me on Discord (`watchthelight`).

This branch is where the Intel side of NvGfx comes together: a kernel driver for Intel Xe GPUs, the same Metal-to-Vulkan plugin that runs NVIDIA cards here, and Mesa's ANV Vulkan driver in between. The aim is one driver for every Xe part (Tiger Lake to Arrow Lake and Arc first, then Lunar Lake, Battlemage and Panther Lake) on macOS 11 to 26.

Status on 2026-10-21: the kernel driver builds and links, its first hardware run is queued, and ANV builds for macOS and passes a simulated test on every Intel generation. Nothing has rendered on Intel hardware with this branch yet; [VedCloud's Meteor Lake port](https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics) has, with a similar stack on macOS 15. Progress is tracked in [XeGfx's issues](https://github.com/watchthelight-labs/XeGfx/issues) and in [issue #12](https://github.com/watchthelight-labs/NvGfx/issues/12) here.

## What lives where

| Path | What it is |
|---|---|
| `gpu/xegfx/` | [XeGfx](https://github.com/watchthelight-labs/XeGfx) as a git subtree (BSD-2): the FreeBSD kernel API on XNU, the build harness, the checks. drm core and i915 come unchanged from FreeBSD's drm-kmod. See its [HOW-IT-WORKS](https://github.com/watchthelight-labs/XeGfx/blob/main/docs/HOW-IT-WORKS.md). |
| `gpu/xkpi/` | The build overlay: replacements for the few drm-kmod files that are GPL-only, so the kext contains no GPL-only code. Provenance in `gpu/xkpi/NOTICE.md`. |
| `gpu/userland/` | How to build Mesa ANV and libdrm for macOS (`BUILD.md`, `scripts/`), the Darwin patches, and the `vkprobe` test. |
| `kexts/NMIntelFB/` | A flip-only framebuffer for the built-in panel that adopts the mode the firmware lit (untested). |
| `kexts/IGPUConsoleRange/` | Lets macOS's own framebuffer driver use a firmware console that sits in stolen memory (tested on Arrow Lake in Hybrid mode). |
| `plugin/`, `kexts/NVRM/accel/` | The Metal plugin and accelerator. The plugin already loads ANV for Intel accelerators; the Intel accelerator build is the next step. |
| `build/nvgfx/build_xegfx.sh` | Builds the Intel kext with the overlay and every check. |

## Building the kernel driver

Sources go in `~/src/xe-src` (`linux`, `freebsd`, `drm-kmod` at `c16de0f`, and `linux-firmware` with `mtl_guc_70.bin` and `LICENSE.i915`), plus [kcabi](https://github.com/watchthelight-labs/kcabi) in `~/src/kcabi`. Then:

```
build/nvgfx/build_xegfx.sh
```

It compiles every source (418 today), refuses GPL-only sources or headers, links against public kernel interfaces only, and fails if the kext redefines a kernel symbol. Installing and testing: `gpu/xegfx/CONTRIBUTING.md`.

## Building ANV

See `gpu/userland/BUILD.md`. In short: libdrm `b97cbde` and Mesa `bffd919` with the two Darwin patches, `-Dvulkan-drivers=intel`, LLVM only at build time. The current futex code needs macOS 14.4 or later; older systems need a fallback (XeGfx issue tracker).

## Built on, and learned from

XeGfx stands on other people's work. Thank you to all of them.

**Code XeGfx builds or ships** (their licences apply to their files):

| Project | What we use |
|---|---|
| [freebsd/drm-kmod](https://github.com/freebsd/drm-kmod) | drm core, TTM, dma-buf and i915 sources, compiled unchanged |
| [FreeBSD LinuxKPI](https://github.com/freebsd/freebsd-src/tree/main/sys/compat/linuxkpi) and [FreeBSD kernel](https://github.com/freebsd/freebsd-src) | the Linux kernel API layer, plus `subr_eventhandler.c` and FreeBSD's headers |
| [Linux](https://github.com/torvalds/linux) i915 and DRM | the driver drm-kmod tracks; the `sync_file` uapi header |
| [linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware) | Intel's GuC firmware (`LICENSE.i915`) |
| [nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver) | the Metal plugin and accelerator NvGfx forks; its old AMD/Intel port (commit `bf2dee3`) supplies the GPL-free dma-buf, sync_file and fence-unwrap replacements and the Mesa/libdrm Darwin patches used in NvGfx's `xe` branch |
| [Mesa](https://gitlab.freedesktop.org/mesa/mesa) ANV and [libdrm](https://gitlab.freedesktop.org/mesa/drm) | the planned Vulkan userspace |
| [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan) | the Metal-to-SPIR-V translator inside the NvGfx plugin |
| [watchthelight-labs/kcabi](https://github.com/watchthelight-labs/kcabi) | kernel collection link and ABI checks |

**Projects we studied** (ideas, findings and warnings only; no code copied):

| Project | What we learned |
|---|---|
| [b00t0x/WhateverXe](https://github.com/b00t0x/WhateverXe), [WhateverXe-Graphics](https://github.com/b00t0x/WhateverXe-Graphics), [b00t0x/OpenCore-Legacy-Patcher](https://github.com/b00t0x/OpenCore-Legacy-Patcher) | Xe-LP on Apple's Tiger Lake driver across Big Sur to Tahoe; forcing BAR0 MMIO to strong uncached; stolen-memory decoding; adopting the firmware's pipes without a modeset |
| [sgiammori/NootedGreen](https://github.com/sgiammori/NootedGreen) | how far the Apple TGL donor approach reaches on Alder Lake and Raptor Lake |
| [JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe) | keeping the firmware's eDP mode, the boot-programmable mode trap in WindowServer, the GGTT[0] machine-check hazard, Tahoe IOSurface offsets |
| [m4j2rpf766-crypto/IntelIGPU-Tahoe](https://github.com/m4j2rpf766-crypto/IntelIGPU-Tahoe) | handing the panel over from IONDRVFramebuffer on Tahoe; isolating the iGPU at boot |
| [VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics](https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics) (earlier: [VedCloud2/CoreUltraGraphics](https://github.com/VedCloud2/CoreUltraGraphics)) | the first i915-on-XNU port to drive real Intel hardware: an i915-derived kernel driver on LinuxKPI, Mesa ANV and a NullMoth-derived Metal plugin, with Metal and Vulkan working on a Meteor Lake Dell Latitude 5350 under macOS 15 |
| [IT-EXPRESS-Bayern/LunarBridge-Hackintosh](https://github.com/IT-EXPRESS-Bayern/LunarBridge-Hackintosh) | Lunar Lake (Xe2) groundwork on Tahoe |
| [wallali77666-cyber/iris-xe-macos-driver](https://github.com/wallali77666-cyber/iris-xe-macos-driver) | a NullMoth-style stack aimed at Tiger Lake |
| [lutzbichler/drm-kmod](https://github.com/lutzbichler/drm-kmod) (xe patches) and [drm-kmod #316](https://github.com/freebsd/drm-kmod/issues/316) | the state of the xe driver outside Linux |
| [drm-kmod #315](https://github.com/freebsd/drm-kmod/issues/315), [#418](https://github.com/freebsd/drm-kmod/pull/418), [#468](https://github.com/freebsd/drm-kmod/pull/468) | LinuxKPI bugs FreeBSD already fixed for Meteor Lake and Arc, to carry over |
| [openbsd/src](https://github.com/openbsd/src) inteldrm | attaching i915 in stages and deferring the heavy probe |
| [acidanthera/WhateverGreen](https://github.com/acidanthera/WhateverGreen) and [OpenCorePkg](https://github.com/acidanthera/OpenCorePkg) | the Intel framebuffer background every Hackintosh graphics project builds on |

