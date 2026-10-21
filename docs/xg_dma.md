# dma area: design notes

Files: `shim/src/fbsd/xg_dma.c` (FreeBSD side), `shim/src/xnu/xg_dma.cpp` (IOKit side), `shim/include/xg_dma.h` (the bridge, plain C types).

## Start order

`xg_dma_init()` has to run once from the kext start, after `xg_kern_init()` and before any drm or i915 module init. It sets up the lock the notifier and backlight lists use, fills the stolen memory globals and logs the IOMMU state. The ACPI notifier and backlight entry points panic with a clear message if they run before it.

## DMA addresses

LinuxKPI's own `linux_pci.c` implements `linux_dma_tag_init*`, `lkpi_dma_map_phys`, `lkpi_dma_unmap`, `linux_dma_map_sg_attrs`, `linux_dma_unmap_sg_attrs` and `linux_dma_alloc_coherent`. They sit on FreeBSD busdma: `bus_dma_tag_create` (pci area), then the inline `bus_dmamap_load_phys`, `_sync`, `_unload` and `bus_dmamem_alloc`, which dispatch through the tag's `impl`. So the mapping policy lives in the pci area's busdma implementation, not here.

The policy the shim assumes: a bus address is the physical address (1:1). That is true only while no IOMMU translates the iGPU's DMA. LinuxKPI's `device_iommu_mapped()` always says false, so i915 cannot notice VT-d on its own.

Detection: `xg_dma_iommu_active()` (FreeBSD side) reads `IOMapper::gSystem` through `xg_dma_xnu_iommu_state()`. The values are 0 for no mapper, 1 while the platform has not decided, 2 when a mapper is expected and otherwise a pointer to the live AppleVTD mapper. XeGfx loads late, so "unknown" should not be seen in practice; it is treated as active. On the Legion (2026-10-20, macOS 26.7.1) `ioreg -c AppleVTD` shows no instance, so 1:1 holds there. When the mapper is active, busdma has to map through `IODMACommand` with the device's mapper instead of returning physical addresses, or refuse to attach. The pci area's `bus_dma_tag_create` should check `xg_dma_iommu_active()` and fail with `ENXIO` until that path exists.

i915 also reads `i915_vtd_active()` to turn off stolen memory on gen < 8; that does not matter for Gen12+.

## Stolen memory (DSM)

drm-kmod's `i915_init()` copies `intel_graphics_stolen_base` and `intel_graphics_stolen_size` into `intel_graphics_stolen_res` at module init, before any device probes. On FreeBSD the x86 early PCI quirks fill them at boot. Here `xg_dma_init()` reads config space of 0:2.0 through the `IOPCIDevice` IOPCIFamily already published (no CF8/CFC access, nothing claimed, so no I-093 style reset):

| IP | Base | Size | Used by i915 |
|---|---|---|---|
| Gen11 to Gen12 (ICL, EHL, JSL, TGL, RKL, ADL, RPL) | config 0xC0/0xC4, low 20 bits masked | GGC (0x50) GMS field: 0x00-0xef in 32 MiB steps, 0xf0-0xfe in 4 MiB steps from 4 MiB | yes, `i915_gem_stolen_smem_setup` |
| Graphics IP 12.70+ (MTL, ARL) | same read, for the log only | same GGC decode (valid 0x00-0x04, 0xf0-0xfe) | no: `HAS_LMEMBAR_SMEM_STOLEN` takes DSM from LMEMBAR (BAR2) at offset 8 MiB and sizes it from GGC itself; `GEN6_DSMBASE` (MMIO 0x1080C0) is the direct-access base |
| older than Gen11 | not probed, globals stay 0 | | |

The device list comes from drm-kmod's `pciids.h`, so new IDs arrive with a drm-kmod update. A zero base forces the size to 0, which i915 reads as "no stolen memory".

## Legacy GTT (`intel_gtt_*`)

These are FreeBSD agp_i810 entry points that drm-kmod's `intel_freebsd.c` calls only for `GRAPHICS_VER < 6` (GMCH platforms). `intel_gtt_get` returns a zeroed `struct intel_gtt` (layout copied from `dev/agp/agp_i810.h`) and `intel_gtt_chipset_flush` is a no-op, which is correct for gen6+. The three PTE functions call `xg_unimpl`: reaching them means a pre-Gen6 GPU, which XeGfx does not support.

## ACPI notifiers

FreeBSD's LinuxKPI hooks the `acpi_acad_event` and `acpi_video_event` eventhandlers. XNU gives kexts no such events, so registered `notifier_block`s sit on a list and the IOKit side delivers events with `xg_dma_acpi_notify(XG_DMA_ACPI_AC, 0, online)` or `xg_dma_acpi_notify(XG_DMA_ACPI_VIDEO, 0x80..0x88, 0)`. The event carries the same `acpi_bus_event` as FreeBSD (class `ac_adapter` with type 0, or class `video` with the notify code). Delivery holds the list shared, like Linux's blocking notifier chain, so callbacks may sleep but must not unregister themselves. Nothing feeds events yet; the opregion handler i915 registers then simply never runs, which matches a machine with no AC or hotkey events.

The shim cannot include LinuxKPI headers yet (`linux/list.h` pulls `machine/pmap.h`, which needs per-CPU pieces still missing), so `xg_dma.c` mirrors the first two members of `struct notifier_block` (`notifier_call`, `next`) and all of `struct acpi_bus_event`. Keep them in step with `compat/linuxkpi/common/include/linux/notifier.h` and `acpi/acpi_bus.h`.

`acpi_target_system_state()` returns `ACPI_STATE_S0` (0) until sleep work calls `xg_dma_acpi_set_target_state()`; the Legion runs with sleep off.

## Backlight

`backlight_register` and `backlight_destroy` keep the FreeBSD semantics (lowest free unit, a cdev whose `si_drv1` is the softc, the brightness cached at register time and on every get) without a devfs node. The cdev is allocated here and named `backlight/backlightN`, so the fd area's `make_dev_s` is not involved. The three `backlight_*_desc` kobj descriptors are defined here with a private default method that returns `ENXIO`, like `kobj_error_method`. `xg_dma_backlight_get/set(unit, ...)` run the same path as the `BACKLIGHTGETSTATUS` and `BACKLIGHTUPDATESTATUS` ioctls, for a later IOFramebuffer brightness hook. They need thread context because i915's backlight ops can sleep. A `device_t` with no kobj ops is skipped instead of faulting.
