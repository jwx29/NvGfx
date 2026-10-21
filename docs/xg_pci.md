# xg_pci: newbus and PCI on XNU

`shim/src/fbsd/xg_pci.c` is the FreeBSD side: kobj, devclasses, `device_t`, resources, bus_dma tags and a PCI bus driver. `shim/src/xnu/xg_pci.cpp` is the IOKit side: it wraps each `IOPCIDevice` and does config space, BAR ranges, BAR mappings and interrupt sources. They talk through `shim/include/xg_pci_xnu.h`, which uses plain C types only.

## The tree

```
root0                      xg_root_driver, also root_bus (LinuxKPI's linux_root_device)
  pcib0                    host bridge, holds the root PCI buses
    pci0                   xg_pci_bus_driver: every pci_* and bus_* method for its children
      hostb0               0:0.0 (class 06/00), so lkpinew_pci_dev() on it gets a name
      pcib1                a PCI-PCI bridge, ivars = its pci_devinfo
        pci1               the bus behind it (busno = secondary bus)
          (driverless)     every other function: ivars, BARs and rid 0 IRQ, no driver
      vgapci0              only the function IOKit handed over (xg_pci_xnu_start)
        drmn0              LinuxKPI drm drivers (name "drmn") attach here
```

`xg_pci_xnu_start(IOPCIDevice *)` enumerates every `IOPCIDevice` once, sorted by bus/device/function so a bridge comes before the bus it leads to, and adds each as a driverless child of its bus (`xg_pci_bus_add`). Then it puts vgapci on the given device (`xg_pci_bus_claim`), which adds `drmn` and probes it. A LinuxKPI `pci_register_driver()` with name "drmn" goes into the vgapci devclass; whether it registers before or after the claim, `drmn0` gets probed. vgapci is never attached to a device that was not handed over, so the NVIDIA GPU (or any other display function) stays untouched.

`pci_find_dbsf`, `pci_find_device`, `pci_find_class_from` and friends search every function found, which is what i915 needs to read the host bridge at 0:0.0. `pci_find_pcie_root_port` walks the real bridge chain; for the integrated GPU it returns NULL, as on FreeBSD.

## What is safe on the live iGPU

- BARs come from IOKit's assigned ranges (`getDeviceMemoryWithRegister`). Nothing sizes a BAR (no 0xffffffff writes), nothing moves one, and decoding is never turned off. The raw BAR is only read to learn its type.
- Enumeration reads config space only.
- `pci_set_powerstate` skips the write when the state does not change, as FreeBSD does.
- Activating a memory BAR maps it uncached (`kIOMapInhibitCache`), like nexus on amd64, and sets MEMEN (already set on a running device). `RF_UNMAPPED` resources are not mapped.

## Interrupts

- rid 0 is INTx, when IOKit has a non-message interrupt index for the function. Its number is the intline when free, else the next free number below 256.
- `pci_alloc_msi`/`pci_alloc_msix` take message interrupt indexes from IOKit, calling `configureInterrupts()` only when fewer exist than asked for. IOPCIFamily owns the capability registers (address, data, enable); FreeBSD's bookkeeping (`msi_alloc`, `msi_handlers`, rids 1..n) is kept. Numbers are `first_msi_irq` (256) and up, `num_msi_irqs` is 256. Release returns the rids and numbers; the IOKit vectors stay configured.
- `bus_setup_intr` creates one `IOWorkLoop` and one `IOInterruptEventSource` per vector. A handler (FreeBSD's ithread half, which is all LinuxKPI uses) runs on that work loop thread and may take sleep mutexes. A filter runs in primary interrupt context through `IOFilterInterruptEventSource` and must not block; `FILTER_SCHEDULE_THREAD` runs the handler.
- One handler per IRQ: IOKit registers one per index, so a second `bus_setup_intr` on the same IRQ returns EBUSY (no `RF_SHAREABLE` sharing).
- `bus_teardown_intr` waits for a running handler (it takes the work loop gate). Do not call it, or `synchronize_irq()`, from that IRQ's own handler.
- `_intr_drain(irq)` (`synchronize_irq`) runs a no-op action on the vector's work loop, so it returns once a running handler has finished.
- `bus_bind_intr` returns EOPNOTSUPP: XNU gives a kext no interrupt affinity control.

## DMA

`bus_dma_tag_create` and friends implement the x86 `bus_dma_impl` without bounce pages: the bus address is the physical address. The PCI bus tag (what `bus_get_dma_tag` returns) has FreeBSD's 4 GB boundary. A child tag narrows lowaddr, boundary and alignment. `bus_dma_id_mapped` is true when the buffer is below lowaddr and aligned, which is the LinuxKPI fast path. A buffer the device cannot reach fails with ENOMEM instead of bouncing. This is only correct while the IOMMU is off (IOMapper disabled); with VT-d on, DMA needs an IODMACommand-based impl (a later milestone). `load_ma` returns EOPNOTSUPP because only `bus_dmamap_load_ma()` in subr_bus_dma.c uses it, and that file is not built. `load_buffer` translates kernel VAs with `pmap_kextract` (vm area).

## Locks

- `bus_topo_lock()` is a recursive sleep mutex of its own, not Giant. FreeBSD drops Giant when a thread sleeps; this one stays held, so i915's probe holds it the whole time, the same as FreeBSD minus that drop. A worker that takes the topology lock while the probe thread waits on it would deadlock. Nothing in LinuxKPI or i915 does that today.
- kobj compiles under its own mutex. The IRQ table has a sleep mutex that is held across IOKit setup and teardown.
- Init is lazy (`xg_pci_once`), but `xg_pci_init()` should run from the kext start before LinuxKPI's `linux_compat_init` reads `root_bus`.

## Not done (later milestones)

- `device_identify`, bus passes, hints, `device_sysctl_*` trees (only the ctx list exists), `devctl` (no devd, so `devctl_notify` drops the event, which is also what FreeBSD does when no reader is listening), resource sharing, PCI domains other than 0, SR-IOV (the `pci_iov_*` descriptors exist only because LinuxKPI's driver class names them), `pci_restore_state`.
