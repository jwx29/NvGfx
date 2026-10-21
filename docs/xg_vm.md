# VM shim design (xg_vm)

`shim/src/fbsd/xg_vm.c` gives drm-kmod, TTM, i915 GEM and LinuxKPI the FreeBSD VM they expect: `struct vm_page`, VM objects with radix tries of pages, a direct map, `pmap_qenter`, `pmap_mapdev_attr`, `kmem_alloc_contig` and the device pager. `shim/src/xnu/xg_vm.cpp` holds the IOKit half behind the C header `shim/include/xg_vm_xnu.h`. Nothing here has run yet. This note records the decisions and what is still open.

## Pages

Consumers use FreeBSD's own `struct vm_page` (from `vm/vm_page.h`) and touch its fields directly through macros: `phys_addr`, `object`, `pindex`, `ref_count`, `busy_lock`, `flags`, `oflags`, `valid`, `dirty`, `md.pat_mode`. So the shim keeps real `struct vm_page` records and does not wrap them.

Every page the shim hands out belongs to a **run**: one IOKit allocation (an `IOBufferMemoryDescriptor`, physically contiguous, `kIOMemoryMapperNone`), or a registered fictitious range, with its `struct vm_page` array. Callers of `vm_page_alloc_noobj_contig` walk `m + i`, so the array has to follow physical order. Runs sit in a pctrie keyed by the physical address of their first page, and `PHYS_TO_VM_PAGE(pa)` is a less-or-equal lookup on it. A run is freed when its last page is freed.

Run kinds:

| kind | made by | CPU access |
|---|---|---|
| pages | `vm_page_alloc_noobj*`, object pages (`vm_page_grab*`) | DMAP (`PHYS_TO_DMAP`) |
| kmem | `kmem_alloc_contig` | the IOBMD's own kernel mapping, with the requested cache mode |
| fictitious | `vm_phys_fictitious_reg_range` (GTT aperture) | none; mapped on demand |

Physical limits: `low`, `high`, `alignment` and `boundary` are checked after the allocation. `high` and `alignment` become the IOBMD physical mask; a block that misses the other limits is released and the call fails.

Busy locking, wiring (`ref_count` with `VPRC_OBJREF`) and fake pages follow `vm_page.c`. There are no page queues and no page daemon: nothing is paged out, `vm_page_unwire` on the last wiring of an object page leaves it in its object, and the last reference frees it. `vm_page_reclaim_contig` reports ENOMEM and `vm_wait` sleeps one tick, so callers retry the way they do on FreeBSD.

The IOBMD per allocation is simple and costs a kernel map entry and an OSObject per page for single-page allocations. A chunked page pool (2 MB IOBMDs split into pages) is the planned change once GEM objects get large.

## Direct map

LinuxKPI's `page_address`, `kmap` and the scatterlist code compute `pa + kva_layout.dmap_low` inline, so a direct map of all RAM is required. XNU x86_64 has one, the physmap. `xg_vm_init` sets `kva_layout.dmap_low` and `dmap_high` from `physmap_base` and `physmap_max`. These two variables are exported by the kernel (tools/kexport.py) but are not declared in the SDK headers; their declaration is copied from `osfmk/i386/pmap.h` in `xg_vm.cpp`. This is the one place the shim relies on a symbol outside the SDK. `km_low`/`km_high` are XNU's kernel map range, so `linux_free_kmem` can tell kmem addresses from DMAP addresses (the physmap sits in a lower PML4 slot).

`kva_layout` is read by consumers before they call into the shim, so it must be set first. A kext constructor in `xg_vm.c` runs `xg_vm_init()`, and the kext start should call it as well (it is idempotent).

The physmap is write-back and cannot be changed from a kext. `pmap_page_set_memattr` records the attribute in `md.pat_mode`, which later mappings use, and writes the page's lines back when it moves to WC or UC, as FreeBSD does when it changes the DMAP. Writes through `kmap` of a WC page still go through the write-back alias. That matches FreeBSD only up to the cache flushes drm does anyway; if it shows up as corruption, `kmap` needs a WC mapping per page.

## Kernel VA

- `kmem_alloc_contig` / `kmem_free`: a kmem run, returned through the IOBMD's kernel address.
- `pmap_mapdev_attr` / `pmap_unmapdev`: `IOMemoryDescriptor::withAddressRanges` over the physical range plus `createMappingInTask(kernel_task, kIOMapAnywhere | cache)`.
- `kva_alloc` reserves VA with an `IOGuardPageMemoryDescriptor` mapping (no access). `pmap_qenter(va, pages, n)` maps each group of pages with the same `pat_mode` over it with `kIOMapOverwrite`, which replaces the guard entries in place (fixed address, `vmf_overwrite` in `IOMemoryDescriptor::memoryReferenceMap`). `pmap_qremove` overwrites the range with a fresh guard mapping (`kIOMapUnique`). `kva_free` releases the reservation, which unmaps the whole range. Overwrite mappings do not unmap when released (`doUnmap` skips them), so their IOKit objects can be dropped once their range is overwritten. This path may block and must not run under a spin mutex.
- `pmap_kextract`: DMAP arithmetic, then the shim's own ranges (a pctrie keyed by kernel address), then IOKit (`withAddressRange` + `prepare` + `getPhysicalSegment`), which may block.

Unverified on hardware: overwrite mappings in `kernel_map` on Tahoe, and IOKit physical descriptors over managed RAM pages. Both are the first things to test at X3.

## Objects

`vm_pager_allocate` makes swap (shmem), phys and SG objects. Swap objects have no backing store, so a missing page is zero filled (`vm_page_grab_valid`). `cdev_pager_allocate` / `cdev_pager_lookup` keep device objects in a list by handle, under an sx lock that also covers the constructor and the final release, so a lookup never revives an object at refcount zero. `vm_object_deallocate` runs the pager destructor and frees the pages; wired pages lose the object reference and are freed by their last unwire. Object page tries are FreeBSD's `vm_radix`, and the pctrie code is FreeBSD's `kern/subr_pctrie.c` compiled in place (only its SMR lock-free lookups are cut out; nothing in the GPU stack uses them).

## User memory (X5)

None of this exists yet, and each entry point fails cleanly instead of panicking, because userspace can reach them:

- `vm_mmap_object` (DRM mmap through the cdev pager) returns ENODEV, `vm_map_find` (legacy `I915_GEM_MMAP`) returns `KERN_NO_SPACE`.
- `vm_fault_quick_hold_pages` returns -1 and `pmap_extract_and_hold` NULL, so userptr and `get_user_pages` fail with EFAULT.
- `pmap_page_is_mapped` is false and `pmap_remove_all` does nothing, which is true while no user mappings exist.

What GEM mmap needs from the XNU side:

1. Mapping. XNU has no kext API for a lazily faulted device pager, so the cdev pager object is populated eagerly: call `cdev_pg_populate` (MGTDEVICE, i915's `vm_fault` path) or `cdev_pg_fault` (DEVICE) for every page of the range at mmap time, collect the physical addresses, build an `IOMemoryDescriptor` over them (`kIOMemoryTypePhysical64`) and map it with `createMappingInTask(current_task(), kIOMapAnywhere | cache)`, the cache mode from the object's `memattr` (WC for GTT and WC mmaps). Swap objects (`vm_map_find`) map their wired shmem pages the same way.
2. Lifetime. The `IOMemoryMap` has to live until the process unmaps, and the pages must not be freed while it exists. That needs a per-mapping record holding the map, a reference on the VM object and a wiring on each page, released from a hook when the task unmaps or exits (the DRM node's close or an `IOUserClient` teardown, depending on how the fd area exposes the node).
3. Invalidation. `zap_vma_ptes`, `unmap_mapping_range` and i915's fence and GTT revocation call `pmap_remove_all` / `cdev_mgtdev_pager_free_pages` to drop user PTEs. With eager mappings this means redirecting or tearing down the `IOMemoryMap` (`IOMemoryMap::redirect` or `unmap`) and making `pmap_page_is_mapped` report the mapping.
4. Faults. Lazy faults (needed for GTT mmaps whose backing moves) would need a real XNU memory object pager, which kexts cannot create. Eager mapping plus redirect on revocation is the plan; the fallback is mapping the object through an `IOUserClient::clientMemoryForType` path in the fd area.

User page holds (userptr) need `IOMemoryDescriptor::withAddressRange(addr, len, dir, current_task())` plus `prepare()` to wire, a per-hold run in the page trie so `PHYS_TO_VM_PAGE` finds the pages, and `complete()` when the last page is unwired.

## Known limits

- A thread sleeping on the busy lock of a page that gets freed wakes up on freed memory if its run is destroyed at the same time. FreeBSD avoids this with a permanent page array; here it relies on callers holding a reference, as FreeBSD's comments already require.
- `pmap_invalidate_cache` runs `wbinvd` on the current CPU only (`mp_rendezvous` is not in the SDK). drm reaches it only on CPUs without clflush.
- `copyin_nofault` / `copyout_nofault` copy with XNU's faulting copyin unless the thread is in a critical section, where they return EFAULT.
- `uma_zcache_create` is not implemented: cache zones need `uma_zalloc` to call the import function, which `xg_mem.c` zones do not do. Only LinuxKPI's `dma_pool` uses it.
- `vm_free_count` reports half of RAM minus what the shim holds; XNU's free count is not in the KPI.
