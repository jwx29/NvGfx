# XeGfx

> **Heads up:** a good portion of this code is AI generated. I do understand how it works,
> and I'm happy to explain any of it. If something looks wrong or you have a question,
> open an issue or ping me on Discord (`watchthelight`).

XeGfx is an attempt at hardware-accelerated graphics for Intel Xe (Gen12 and later) GPUs on macOS. Tiger Lake to Arrow Lake come first; Lunar Lake and later (Xe2, which only the Linux `xe` driver supports) are a later phase.

The approach is to reuse a driver that already works rather than write one. FreeBSD's [drm-kmod](https://github.com/freebsd/drm-kmod) runs the Linux i915 driver on a non-Linux kernel through LinuxKPI, a layer that implements the Linux kernel API on top of FreeBSD's. XeGfx puts a small FreeBSD-kernel-API layer for XNU underneath LinuxKPI, so drm core and i915 can build as a kext. On top of that sit a DRM node for userspace, so Mesa's ANV Vulkan driver can run unchanged, and a flip-only IOFramebuffer plus IOAccelerator glue for WindowServer. The panel is lit by the firmware (GOP), so the first desktop needs no mode setting; i915's display code stays disabled until external monitors.

## Status

Work in progress. Nothing links or loads yet.

What works today: every drm, dma-buf, ttm, i915 and LinuxKPI source (411 files) compiles to an object for the macOS kernel target. Linking those objects first left about 400 FreeBSD kernel functions that XNU does not export and XeGfx has to implement itself (`docs/kpi-todo-baseline.txt`). The first XNU implementations are in `shim/src/fbsd` (threads, per-CPU data, clock, tunables, malloc and uma, mutexes, sx and rw locks, condition variables and sleep queues), which brings it to 322. None of it has run yet.

Progress is posted in [NvGfx issue #12](https://github.com/watchthelight-labs/NvGfx/issues/12).

## Milestones

| M | Done when |
|---|---|
| X1 compiles | every drm core and i915 GT file compiles for the kernel target |
| X2 links | one kext links with no undefined symbols outside XNU KPIs |
| X3 loads | it loads next to the firmware framebuffer, probes the GT only and does not touch the display |
| X4 GT up | GuC/HuC loaded (or execlists), a request completes, the render node exists |
| X5 ANV | Vulkan device enumerates, compute and an offscreen draw pass |
| X6 desktop | the WindowServer Metal compositor runs on the Intel device and flips through PLANE_SURF |
| X7 others | builds and boots on at least one other Xe laptop, with docs |

X1 is done. X2 is the current work. Any milestone can end in "not feasible", and that would be written up here.

## Layout

| Path | What it is |
|---|---|
| `harness/score.py` | Builds every drm-kmod source for the XNU kernel target and ranks what breaks, or with `--obj` builds objects and lists undefined symbols. |
| `tools/kpi_split.py` | Splits those undefined symbols into the ones the running macOS kernel already exports and the ones XeGfx must provide. |
| `shim/include` | XNU overrides of FreeBSD and LinuxKPI headers. |
| `shim/src/fbsd` | The FreeBSD kernel API implemented on XNU. |
| `shim/gen` | Headers generated from FreeBSD's `*_if.m` interface files, plus empty kernel option headers. |
| `docs/` | Baseline lists the tools produce. |

## Building

The third-party sources are not in this repo. The scripts expect sparse clones in `~/src/xe-src`:

- `linux`: the Linux kernel tree
- `freebsd`: the FreeBSD source tree (LinuxKPI lives in `sys/compat/linuxkpi`)
- `drm-kmod`: FreeBSD's drm-kmod

You need the Xcode Command Line Tools (clang and the macOS SDK) and Python 3.

```
python3 harness/score.py         # syntax-only pass over every source, scoreboard in out/score.json
python3 harness/score.py --obj   # build objects in out/obj, undefined symbols in out/undefined.txt
python3 tools/kpi_split.py       # out/kpi-have.txt and out/kpi-todo.txt
```

`tools/kpi_split.py` reads the boot kernel collection with [kcabi](https://github.com/watchthelight-labs/kcabi), which it expects in `~/src/kcabi`.

## Credits

- FreeBSD's drm-kmod and LinuxKPI, which do the hard part of running Linux DRM drivers on another kernel.
- The Linux i915 and DRM developers.
- Mesa's ANV Vulkan driver, the planned userspace.
- NullMoth's NVIDIA driver for macOS, which [NvGfx](https://github.com/watchthelight-labs/NvGfx) forks; it is the model for the macOS side (framebuffer, accelerator and Metal plugin).

## License

The project's own code is under the BSD 2-Clause license (see `LICENSE`). Third-party code keeps its own license; see `NOTICE`.
