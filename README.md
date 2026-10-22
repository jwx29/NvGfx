# XeGfx

XeGfx is an attempt at hardware-accelerated graphics for Intel Xe GPUs on macOS. The goal is one driver for every Xe part (Tiger Lake through Arrow Lake, Arc A-series, then Lunar Lake, Battlemage and Panther Lake) on every macOS from Big Sur 11 to Tahoe 26.

Rather than a new GPU driver written from scratch, it builds on FreeBSD's [drm-kmod](https://github.com/freebsd/drm-kmod), which already runs the Linux i915 driver on a non-Linux kernel through LinuxKPI, a layer that implements the Linux kernel API on top of FreeBSD's. XeGfx puts a small FreeBSD-kernel-API layer for XNU underneath LinuxKPI, so drm core and i915 build as one kext. Userspace is planned as Mesa's ANV Vulkan driver under the Metal-to-Vulkan plugin of [NvGfx](https://github.com/watchthelight-labs/NvGfx), the same way that project runs NVIDIA GPUs.

## Built on, and learned from

XeGfx is built on other people's work. Thanks to all of them.

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
| [b00t0x/WhateverXe](https://github.com/b00t0x/WhateverXe), [WhateverXe-Graphics](https://github.com/b00t0x/WhateverXe-Graphics), [b00t0x/OpenCore-Legacy-Patcher](https://github.com/b00t0x/OpenCore-Legacy-Patcher) | Xe-LP on Apple's Tiger Lake driver across Big Sur to Tahoe; forcing BAR0 MMIO to strong uncached; stolen-memory decoding; adopting the firmware's pipes without a modeset; since 0.4.0 Tiger Lake laptop panels too, by reading panel facts from the firmware's VBT and keeping the eDP scrambler and framing bits in step with the panel during link training; shipping the graphics stack as a signed /Library package instead of System patches |
| [sgiammori/NootedGreen](https://github.com/sgiammori/NootedGreen) | how far the Apple TGL donor approach reaches on Alder Lake and Raptor Lake |
| [JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe](https://github.com/JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe) | keeping the firmware's eDP mode, the boot-programmable mode trap in WindowServer, the GGTT[0] machine-check hazard, Tahoe IOSurface offsets; holding the boot plane dark until WindowServer owns the display, and a scheduler fix for a context-status event that was being dropped |
| [m4j2rpf766-crypto/IntelIGPU-Tahoe](https://github.com/m4j2rpf766-crypto/IntelIGPU-Tahoe) | handing the panel over from IONDRVFramebuffer on Tahoe; isolating the iGPU at boot |
| [VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics](https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics) (earlier: [VedCloud2/CoreUltraGraphics](https://github.com/VedCloud2/CoreUltraGraphics)) | the first i915-on-XNU port to drive real Intel hardware: an i915-derived kernel driver on LinuxKPI, Mesa ANV and a NullMoth-derived Metal plugin, with Metal and Vulkan working on a Meteor Lake Dell Latitude 5350 under macOS 15; GuC submission with Linux's own MTL firmware, a GPU-composited desktop, and two lessons for our LinuxKPI: a lost wake-up that looked like GuC timeouts, and starting the stolen-memory allocator after the firmware's console image |
| [kitsuneislife/nmx-intel](https://github.com/kitsuneislife/nmx-intel) | i915 through NullMoth's port lighting a Raptor Lake laptop panel on macOS, and a framebuffer that copies frames composited on another GPU to the Intel panel |
| [IT-EXPRESS-Bayern/LunarBridge-Hackintosh](https://github.com/IT-EXPRESS-Bayern/LunarBridge-Hackintosh) | Lunar Lake (Xe2) groundwork on Tahoe |
| [wallali77666-cyber/iris-xe-macos-driver](https://github.com/wallali77666-cyber/iris-xe-macos-driver) | a NullMoth-style stack aimed at Tiger Lake |
| [lutzbichler/drm-kmod](https://github.com/lutzbichler/drm-kmod) (xe patches) and [drm-kmod #316](https://github.com/freebsd/drm-kmod/issues/316) | the state of the xe driver outside Linux |
| [drm-kmod #315](https://github.com/freebsd/drm-kmod/issues/315), [#418](https://github.com/freebsd/drm-kmod/pull/418), [#468](https://github.com/freebsd/drm-kmod/pull/468) | LinuxKPI bugs FreeBSD already fixed for Meteor Lake and Arc, to carry over |
| [openbsd/src](https://github.com/openbsd/src) inteldrm | attaching i915 in stages and deferring the heavy probe |
| [acidanthera/WhateverGreen](https://github.com/acidanthera/WhateverGreen) and [OpenCorePkg](https://github.com/acidanthera/OpenCorePkg) | the Intel framebuffer background every Hackintosh graphics project builds on |

## Status (2026-10-21)

Work in progress. XeGfx has not driven a GPU yet. The approach itself is proven on real hardware: [VedCloud's Meteor Lake port](https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics) runs an i915-derived driver on XNU with Metal and Vulkan working on a Dell Latitude 5350 under macOS 15. XeGfx targets Tahoe with its own LinuxKPI shim, so treat every milestone below as open until a boot proves it here.

| M | Done when | State |
|---|---|---|
| X1 compiles | every drm core and i915 source compiles for the kernel target | done (418 of 418) |
| X2 links | one kext links against public KPIs only, no stubs on reachable paths | done |
| X3 loads | loads next to the firmware framebuffer and probes the GPU read-only | built, first hardware test queued |
| X4 GT up | GuC loaded, a request completes, render node exists | next |
| X5 ANV | Vulkan device enumerates, compute and an offscreen draw pass | ANV builds; needs X4 |
| X6 desktop | WindowServer composites on the Intel GPU and flips through PLANE_SURF | |
| X7 others | builds and boots on other Xe laptops, with docs | testers wanted |

Hardware so far: one Lenovo Legion 5 15IAX10 (Arrow Lake-HX, `8086:7D67`, Xe-LPG, display 14) on macOS Tahoe 26.7.1, BIOS in Hybrid mode so the iGPU drives the panel.

## How it stays safe to test

A broken GPU driver can stop a machine before anything reaches the disk, so the kext is built to be harmless until asked:

- It never loads at boot. Its only IOKit personality waits for a resource the kext publishes itself, after `kmutil load -b org.watchthelight.XeGfx`. If a test crashes the Mac, it reboots into a system where XeGfx is not loaded.
- It runs in stages, one sysctl write at a time:

  | `debug.xegfx_stage` | What runs |
  |---|---|
  | 1 | the shim's own state (clock, threads, VM, tunables, task queues) |
  | 2 | a read-only look at the GPU: IDs, BAR sizes, GMD_ID, stolen-memory base, pipe A registers, the GGTT entry behind the panel, PCH watchdog registers, published as the `XeGfxProbe` registry dictionary |
  | 3 | newbus over every PCI device, the DMA layer |
  | 4 | the drm and i915 init routines, i915 display disabled, then a check that the panel still scans out the same memory and pipe A still counts frames |

- A stage that runs longer than `debug.xegfx_wdog` seconds panics on purpose, so a stall becomes a panic report and a reboot instead of a frozen screen.
- `debug.xegfx_log` returns the last 64 KiB of the driver's own console output, and `debug.xegfx_results` the result of each stage.
- `debug.xegfx_env` sets tunables for the next stage (`compat.linuxkpi.i915_enable_guc=3` and the like), so one boot can try several settings.
- The link fails if the kext redefines any symbol the kernel exports (`tools/shadow_check.py`). That bug class killed the first two hardware tests.

## Where development happens

This repository holds the XNU side: the FreeBSD-kernel-API layer (`shim/`), the build harness and the checks. It is pulled as a subtree into the [NvGfx](https://github.com/watchthelight-labs/NvGfx) fork, branch `xe`, under `gpu/xegfx`, and that is where you should build it. The fork adds `gpu/xkpi`, an overlay that replaces the few drm-kmod files that are GPL-only, and the Metal plugin and accelerator glue. A build from this repository alone still uses drm-kmod's GPL-only headers and is only good for development.

## Building

You need the Xcode Command Line Tools (clang and the macOS SDK) and Python 3. The third-party sources are not in the repository; the scripts expect sparse clones in `~/src/xe-src`:

- `linux`: the Linux kernel tree
- `freebsd`: the FreeBSD source tree (LinuxKPI lives in `sys/compat/linuxkpi`)
- `drm-kmod`: FreeBSD's drm-kmod (commit `c16de0f`, Linux 6.14 level)
- `linux-firmware`: `mtl_guc_70.bin` and `LICENSE.i915` (Meteor Lake and Arrow Lake GuC), copied into the kext

From the NvGfx `xe` branch:

```
build/nvgfx/build_xegfx.sh      # objects, licence gate, link, KPI / shadow / kcabi link checks
```

Here, for work on the shim itself:

```
python3 harness/score.py --obj   # objects in out/obj, undefined symbols in out/undefined.txt
bash tools/link.sh               # out/XeGfx.kext
sudo tools/install.sh            # into the auxiliary kernel collection (XEGFX_MODE=uninstall to remove)
```

`tools/kpi_split.py` and the link check use [kcabi](https://github.com/watchthelight-labs/kcabi), expected in `~/src/kcabi`.

## Layout

| Path | What it is |
|---|---|
| `shim/src/fbsd` | The FreeBSD kernel API on XNU: threads, locks, sleep queues, malloc and UMA, VM pages and objects, newbus and PCI, bus_dma, sysctl, taskqueues, callouts, firmware loading |
| `shim/src/xnu` | The C++ side that talks to IOKit: kext entry and stages, PCI devices, interrupts, DMA, devfs nodes |
| `shim/include` | XNU overrides of FreeBSD and LinuxKPI headers; `xg_bsdnames.h` renames FreeBSD functions that share a name with a kernel export |
| `harness/score.py` | Compiles every drm-kmod source for the XNU kernel target; `--obj` builds the objects and runs the licence gate |
| `tools/` | `link.sh`, `kpi_check.py` (public KPIs only), `shadow_check.py`, `install.sh` |
| `docs/` | One design note per shim area (`xg_*.md`) and [HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) |

## Helping

Testers with any Intel Xe machine are the most useful thing right now, and so is anyone who knows XNU, IOKit or i915 internals. See [CONTRIBUTING.md](CONTRIBUTING.md) and the open issues.

## License

The project's own code is under the BSD 2-Clause license (see `LICENSE`). Third-party code keeps its own license; see `NOTICE`.
