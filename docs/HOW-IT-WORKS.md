# How XeGfx works

This is the long version of the README, for anyone who wants to change the code or check that it does what it claims.

## The layers

```
 Metal apps, WindowServer
 NvGfx Metal plugin (Metal -> Vulkan)          userspace
 Mesa ANV  +  libdrm  +  a transport shim
 ------------------------------------------------------------
 DRM device nodes (devfs clone nodes)           kernel, XeGfx.kext
 drm core, TTM, dma-buf, i915                   unchanged drm-kmod sources
 LinuxKPI                                       unchanged FreeBSD sources
 FreeBSD kernel API on XNU                      shim/ (this repository)
 XNU: IOKit, Mach, BSD KPIs
```

Everything above the shim is someone else's code, compiled unchanged. That is the point: i915 is hundreds of thousands of lines that Intel and the Linux community keep current, and FreeBSD already proved the Linux driver can run on a different kernel through LinuxKPI. What XeGfx adds is the bottom layer, the FreeBSD kernel interfaces that LinuxKPI and drm-kmod call, implemented on what a macOS kext is allowed to use.

## The shim, area by area

Each area has its own design note in `docs/`:

- **sched** (`xg_sched.md`): kernel threads on XNU threads, a thread registry so `curthread` works, `sched_pin` and critical sections on a spin lock (no CPU binding, it is not a public KPI), ck_epoch, kernel FPU sections.
- **vm** (`xg_vm.md`): real FreeBSD `vm_page` arrays, backed by IOKit allocations and kept in a pctrie. There is no direct map on macOS, so physical-to-virtual lookups go through the IOKit mappings the shim made. `kva_alloc` takes from a pageable reservation.
- **pci** (`xg_pci.md`): a newbus tree (`root0`, `pcib0`, `pci0`) built over every IOPCIDevice, config space and BARs through IOPCIDevice, MSI through IOInterruptEventSource. LinuxKPI's `vgapci` and `drmn` attach to it the way they do on FreeBSD.
- **dma** (`xg_dma.md`): `bus_dma` tags on IOKit memory, the stolen-memory probe, GTT entry points, ACPI notifiers, the backlight registry.
- **fd** (`xg_fd.md`): character devices as XNU devfs clone nodes, so every `open` of a DRM node gets its own file, as LinuxKPI expects; private file tables; eventfd.
- **misc** (`xg_misc.md`): a private sysctl tree, tunables read from boot-args or from `debug.xegfx_env`, sbuf, counters, task queue groups, firmware loading from the kext's `Resources`, select and knotes.

Rules the shim keeps:

- **Public KPIs only.** No `com.apple.kpi.private`, no unexported kernel symbols. `tools/kpi_check.py` enforces it at link time.
- **No shadowing of kernel symbols.** The FreeBSD and Linux code is linked statically into the kext, so any function it defines with the same name as a kernel export captures the kext's own XNU-side callers. That is how the first hardware tests died: `_start` registered an XNU sysctl through FreeBSD's `sysctl_register_oid` and dereferenced NULL. `shim/include/xg_bsdnames.h` renames the FreeBSD copies for every FreeBSD and Linux compilation unit, and `tools/shadow_check.py` fails the link if a new clash appears.
- **No GPL-only code in the shipping build.** The harness records every header each object includes and refuses (with an overlay set) to build if any compiled source or included header is GPL-only. The NvGfx fork's overlay supplies replacements for drm-kmod's few GPL-only files.

## Load and start

1. The kext is in the auxiliary kernel collection but nothing loads it at boot. Its personality matches IOResources only once the `XeGfxArm` resource exists.
2. `kmutil load -b org.watchthelight.XeGfx` runs `_start`, which allocates two locks and a watchdog thread call, registers the `debug.xegfx_*` sysctls with the kernel and publishes `XeGfxArm`. IOKit then attaches `XeGfxResident` to IOResources. Nothing touches hardware.
3. `sysctl debug.xegfx_stage=N` runs the stages up to N that have not run yet, in order, in the calling thread. Each stage arms the watchdog for `debug.xegfx_wdog` seconds and records its result (1 pass, negative failure) in `debug.xegfx_results`.

Stage 2 never maps BAR2 or touches stolen memory directly. On Meteor Lake class parts, CPU access to stolen memory through a BAR can hang the machine (i915's `Wa_22018444074`). It reads registers through an uncached BAR0 mapping only, and reads the GGTT entry that maps the firmware's framebuffer (GGTT sits at BAR0 + 8 MiB on Gen12 and later).

Stage 4 runs every SYSINIT in the kext, the way FreeBSD's module loader does: drm, TTM, dma-buf, then i915, which finds the GPU through newbus and probes it. i915's display code is off (`compat.linuxkpi.i915_disable_display=1` by default), because the firmware already lit the panel and macOS's own framebuffer driver uses it. Afterwards the stage reads the GGTT entry behind the panel's surface again and samples pipe A's frame counter. If i915 rewrote the entry or the pipe stopped, the stage fails with "display lost". WindowServer would look fine in that state while the screen is black, so the check matters.

## Firmware

Gen12 and later need Intel's GuC firmware for command submission (from Alder Lake-P on, in practice also on Meteor Lake and Arrow Lake). The kext carries the blobs it needs in `Contents/Resources` under their base names (`mtl_guc_70.bin`), with Intel's `LICENSE.i915`. LinuxKPI's `request_firmware` asks for `i915/mtl_guc_70.bin`, the shim retries with the base name, and the XNU side loads it through `OSKextRequestResource`. That request only completes after `_start` has returned, which is one more reason the stages run later.

## Testing on a real machine

The Legion setup uses a LaunchDaemon (`xe-guard` in the private project repo) that runs queued experiments after login and judges them without a person watching: it takes a queue entry before touching anything, loads the kext, runs the stages, snapshots `debug.xegfx_log`, the probe dictionaries and the kernel log after each stage, and calls it a pass only if every stage passed, WindowServer kept its process, and no panic report appeared. You can do the same by hand:

```
sudo kmutil load -b org.watchthelight.XeGfx
sudo sysctl debug.xegfx_wdog=60
sudo sysctl debug.xegfx_stage=2
ioreg -l -w0 -r -c XeGfxResident | grep -A40 XeGfxProbe
sysctl -b debug.xegfx_log
```

## What is not done

- The DRM render node exists in the code but has not been exercised; ANV needs a small userspace transport shim (sync_file emulation, mapping by ioctl, PCI identity) before it can talk to it.
- The Metal side (an accelerator and the plugin's Intel path) lives in NvGfx and has not run on Intel yet.
- Display ownership (a flip-only framebuffer that adopts the firmware's mode) is a separate kext in NvGfx (NMIntelFB) and is untested.
- The xe driver (Lunar Lake, Battlemage, Panther Lake) is not built yet; it comes after i915 works.
