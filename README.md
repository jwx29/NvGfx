# XeGfx

XeGfx is an open source Intel graphics driver for macOS: a kext that runs the Linux i915 driver on XNU, so that Hackintosh machines with Intel Xe graphics (Iris Xe, Intel Arc and the Core Ultra iGPUs) can get hardware acceleration instead of a plain framebuffer. On top of it, Mesa's ANV driver already gives a working Vulkan device on an Arrow Lake iGPU under macOS Tahoe 26.7.1. The goal is one driver for every Xe part (Tiger Lake through Meteor Lake and Arrow Lake, Arc A-series, then Lunar Lake, Battlemage and Panther Lake) on every macOS from Big Sur 11 to Tahoe 26.

It is not a GPU driver written from scratch. FreeBSD's [drm-kmod](https://github.com/freebsd/drm-kmod) already runs i915 on a non-Linux kernel through LinuxKPI, a layer that implements the Linux kernel API on top of FreeBSD's. XeGfx adds a small FreeBSD-kernel-API layer for XNU underneath LinuxKPI, so drm core and i915 build as one kext with their sources almost untouched. In userspace, Mesa's ANV Vulkan driver talks to it through a patched libdrm, and the next step is to run the Metal-to-Vulkan plugin of [NvGfx](https://github.com/watchthelight-labs/NvGfx) (branch `xe`) on ANV, the same way that project gives Metal to NVIDIA GPUs.

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
| [Mesa](https://gitlab.freedesktop.org/mesa/mesa) ANV and [libdrm](https://gitlab.freedesktop.org/mesa/drm) | the Vulkan userspace |
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
| [kitsuneislife/nmx-intel](https://github.com/kitsuneislife/nmx-intel) | i915 through NullMoth's port lighting a Raptor Lake laptop panel on macOS, and a framebuffer that copies frames composited on another GPU to the Intel panel; its pitfalls notes taught us to reserve the boot framebuffer's stolen pages and GGTT range before i915 allocates, and to give the framebuffer an `IOFBDependentID` |
| [IT-EXPRESS-Bayern/LunarBridge-Hackintosh](https://github.com/IT-EXPRESS-Bayern/LunarBridge-Hackintosh) | Lunar Lake (Xe2) groundwork on Tahoe |
| [wallali77666-cyber/iris-xe-macos-driver](https://github.com/wallali77666-cyber/iris-xe-macos-driver) | a NullMoth-style stack aimed at Tiger Lake |
| [lutzbichler/drm-kmod](https://github.com/lutzbichler/drm-kmod) (xe patches) and [drm-kmod #316](https://github.com/freebsd/drm-kmod/issues/316) | the state of the xe driver outside Linux |
| [drm-kmod #315](https://github.com/freebsd/drm-kmod/issues/315), [#418](https://github.com/freebsd/drm-kmod/pull/418), [#468](https://github.com/freebsd/drm-kmod/pull/468) | LinuxKPI bugs FreeBSD already fixed for Meteor Lake and Arc, to carry over |
| [openbsd/src](https://github.com/openbsd/src) inteldrm | attaching i915 in stages and deferring the heavy probe |
| [acidanthera/WhateverGreen](https://github.com/acidanthera/WhateverGreen) and [OpenCorePkg](https://github.com/acidanthera/OpenCorePkg) | the Intel framebuffer background every Hackintosh graphics project builds on |

## Status (2026-10-22)

Work in progress. The Intel GPU runs real work for the kernel and for user processes, Vulkan draws offscreen, and Metal now runs on it too: apps get a Metal device on the Intel GPU, and Metal compute and Metal drawing come back correct. Nothing is on screen through it yet. In Hybrid mode WindowServer still draws the desktop without acceleration into the firmware framebuffer.

Everything below was proven on one machine: a Lenovo Legion 5 15IAX10 with an Intel Core Ultra 9 275HX (Arrow Lake-HX). Its integrated graphics are Xe-LPG, PCI ID `8086:7D67` (GMD_ID graphics 12.70, display 14), which i915 treats as part of the Meteor Lake family. It runs macOS Tahoe 26.7.1 (25G241) with the BIOS in Hybrid graphics mode. The boot IDs (N-xx) are test boots on that laptop.

| Milestone | Done when | State |
|---|---|---|
| X1 compiles | every drm core and i915 source compiles for the kernel target | done, all 418 sources |
| X2 links | one kext links against public KPIs only, no stubs on reachable paths | done |
| X3 loads | loads next to the firmware framebuffer and probes the GPU | done, N-219 |
| X4 GT up | GuC loaded, a request completes, render node exists | done, N-229 |
| X5 ANV | a Vulkan device, a compute pipeline and an offscreen draw | done, N-234 (2026-10-22) |
| X5b Metal | apps get a Metal device on the Intel GPU; compute and a draw come back right | done, N-235 to N-238 (2026-10-22) |
| X6 desktop | WindowServer composites on the Intel GPU and the Intel GPU shows it | not yet |
| X7 other laptops | builds and boots on other Xe machines, with docs | testers wanted |

What each step showed on the 7D67:

- **GT up (N-229).** The GuC firmware `mtl_guc_70.bin` (version 70.53.0) loads, with GuC submission, SLPC and RC enabled. A kernel request completed on each of the render, copy and compute engines (rcs0, bcs0, ccs0), and each one wrote a dword that the CPU read back.
- **Userspace (N-230).** A user process opened `/dev/dri/renderD128`, created a context, mapped buffers and ran execbuffer on the render and copy engines, and the results read back correctly.
- **libdrm (N-233).** `drmGetDevices2` lists the GPU as `0000:00:02.0 8086:7d67`, driver i915 1.6.0.
- **Vulkan through Mesa ANV (N-234).** ANV creates a Vulkan 1.4 device (API 1.4.363), "Intel(R) Graphics (ARL)", and a compute pipeline on it. Right after that boot, a draw test (`tools/vkdraw` on NvGfx `xe`, commit `9c9cbc3`) cleared a 64x64 image to red in a render pass, drew a green triangle into it (exactly 512 green pixels), copied the image to a host buffer, and the CPU read back the right pixels.

- **Metal (N-235 to N-238).** XeGfx's stage 7 hands the GPU to XeGfxAccel, the NMXAccel accelerator from NvGfx built over XeGfx (`build/nvgfx/build_nvaccel.sh xegfxaccel` on NvGfx `xe`). The NvGfx Metal plugin then runs on ANV. Metal shows one device, "Intel(R) Graphics (ARL)". An MSL compute kernel fills 1024 values correctly, and a render pipeline draws a green triangle on red that reads back exactly. Getting there took three fixes. Render nodes are world-accessible, as on Linux. GFP_ATOMIC allocations get a reserve, because i915 allocates under spinlocks. Host-visible BOs use the GPU's snooping write-back PAT, because XNU keeps RAM write-back whatever cache mode an IOKit mapping asks for, and write-combining BOs lost cache lines to CPU write-backs.

Next is the desktop: WindowServer compositing on the Intel GPU through this Metal device and the panel scanned out from it.

The current build is the [v0.0.22 prerelease](https://github.com/watchthelight-labs/XeGfx/releases/tag/v0.0.22) (XeGfx commit `dac3979`, with the libdrm patch `gpu/userland/patches/libdrm-xegfx.patch` from NvGfx `xe` commit `b020742`). It is a test build for people who want to help, not something to install on a machine you depend on.

[VedCloud's Meteor Lake port](https://github.com/VedCloud/Meteor-Lake-Xe-LPG-Metal-Graphics) got there first: it ran an i915-derived driver with Metal and Vulkan on a Dell Latitude 5350 under macOS 15. XeGfx takes a different road (unchanged drm-kmod sources over its own shim) and targets Tahoe as well.

### Problems worth knowing about

- drm-kmod never turns the GuC on in FreeBSD builds, and Meteor Lake and Arrow Lake cannot submit work without it, so XeGfx sets `enable_guc=3` by default.
- `OSKextRequestResource` does not serve the `Resources` of a kext that lives in the auxiliary kernel collection, so the GuC firmware is linked into the kext binary.
- XNU devfs clone nodes give every `stat` and `open` its own minor number, so libdrm could not match an open file to its `/dev/dri` node. The patched libdrm matches nodes by slot instead.

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
- `linux-firmware`: `mtl_guc_70.bin` and `LICENSE.i915` (Meteor Lake and Arrow Lake GuC), linked into the kext

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
