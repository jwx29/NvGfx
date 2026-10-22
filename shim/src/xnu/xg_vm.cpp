/*
 * XeGfx vm: IOKit memory for the FreeBSD VM shim (physical page runs, kernel mappings with a cache mode, VA
 * reservations that pmap_qenter fills in place).
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <mach/vm_param.h>
#include "xg_vm_xnu.h"

/* osfmk/i386/pmap.h: the bounds of XNU's direct map; exported by the kernel but not declared in the SDK */

struct xg_vm_xmem {
	IOMemoryDescriptor *md;
	IOMemoryMap *map;
};

static IOOptionBits
xg_vm_cacheopt(int cache)
{
	switch (cache) {
	case XG_VM_CACHE_WB:
		return kIOMapCopybackCache;
	case XG_VM_CACHE_WC:
		return kIOMapWriteCombineCache;
	case XG_VM_CACHE_UC:
		return kIOMapInhibitCache;
	case XG_VM_CACHE_WT:
		return kIOMapWriteThruCache;
	default:
		return kIOMapDefaultCache;
	}
}

static xg_vm_xmem_t
xg_vm_xnew(IOMemoryDescriptor *md, IOMemoryMap *map)
{
	xg_vm_xmem_t xm = (xg_vm_xmem_t)IOMallocZero(sizeof(*xm));

	if (xm == nullptr) {
		if (map)
			map->release();
		if (md)
			md->release();
		return nullptr;
	}
	xm->md = md;
	xm->map = map;
	return xm;
}

extern "C" xg_vm_xmem_t
xg_vm_x_alloc(unsigned long long size, unsigned long long physmask, int cache, void **kva, unsigned long long *pa)
{
	IOBufferMemoryDescriptor *bmd;
	IOByteCount len = 0;
	addr64_t p;

	/* MapperNone: physmask and the returned address are host physical, not VT-d addresses */
	bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut |
	    kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone | xg_vm_cacheopt(cache), size, physmask);
	if (bmd == nullptr)
		return nullptr;
	p = bmd->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
	if (p == 0 || len < size) {
		IOLog("XeGfx: vm: buffer of %llu bytes is not contiguous (%llu)\n", size, (unsigned long long)len);
		bmd->release();
		return nullptr;
	}
	*kva = bmd->getBytesNoCopy();
	*pa = p;
	return xg_vm_xnew(bmd, nullptr);
}

extern "C" xg_vm_xmem_t
xg_vm_x_map_phys(unsigned long long pa, unsigned long long size, int cache, void **kva)
{
	IOAddressRange r = { pa, size };
	IOMemoryDescriptor *md;
	IOMemoryMap *map;

	md = IOMemoryDescriptor::withAddressRanges(&r, 1, kIODirectionInOut | kIOMemoryTypePhysical64 |
	    kIOMemoryMapperNone, nullptr);
	if (md == nullptr)
		return nullptr;
	map = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | xg_vm_cacheopt(cache), 0, size);
	if (map == nullptr) {
		md->release();
		return nullptr;
	}
	*kva = (void *)map->getVirtualAddress();
	return xg_vm_xnew(md, map);
}

static xg_vm_xmem_t
xg_vm_map_ranges(task_t task, const unsigned long long *pa, const unsigned long long *len, unsigned int n, int cache,
    int writable, unsigned long long *addr);

extern "C" xg_vm_xmem_t
xg_vm_x_map_user(const unsigned long long *pa, const unsigned long long *len, unsigned int n, int cache, int writable,
    unsigned long long *uaddr)
{
	/* the ioctl runs on the caller's thread, so current_task() is the process that asked */
	return xg_vm_map_ranges(current_task(), pa, len, n, cache, writable, uaddr);
}

/* a kernel mapping at an address IOKit picks; overlaying a reserved range (xg_vm_x_map_at) fails inside the pageable
 * submaps that back kva_alloc (N-221) */
extern "C" xg_vm_xmem_t
xg_vm_x_map_kernel(const unsigned long long *pa, const unsigned long long *len, unsigned int n, int cache, void **kva)
{
	unsigned long long a;
	xg_vm_xmem_t xm = xg_vm_map_ranges(kernel_task, pa, len, n, cache, 1, &a);

	*kva = xm ? (void *)a : nullptr;
	return xm;
}

static xg_vm_xmem_t
xg_vm_map_ranges(task_t task, const unsigned long long *pa, const unsigned long long *len, unsigned int n, int cache,
    int writable, unsigned long long *addr)
{
	IOAddressRange *r;
	IOMemoryDescriptor *md;
	IOMemoryMap *map;
	mach_vm_size_t size = 0;

	if (n == 0 || (r = (IOAddressRange *)IOMallocZero(n * sizeof(*r))) == nullptr)
		return nullptr;
	for (unsigned int i = 0; i < n; i++) {
		r[i].address = pa[i];
		r[i].length = len[i];
		size += len[i];
	}
	md = IOMemoryDescriptor::withAddressRanges(r, n, kIODirectionInOut | kIOMemoryTypePhysical64 |
	    kIOMemoryMapperNone, nullptr);
	IOFree(r, n * sizeof(*r));
	if (md == nullptr)
		return nullptr;
	map = md->createMappingInTask(task, 0, kIOMapAnywhere | xg_vm_cacheopt(cache) | (writable ? 0 : kIOMapReadOnly),
	    0, size);
	if (map == nullptr) {
		md->release();
		return nullptr;
	}
	*addr = map->getAddress();
	return xg_vm_xnew(md, map);
}

extern "C" xg_vm_xmem_t
xg_vm_x_reserve(unsigned long long size, void **kva)
{
	IOBufferMemoryDescriptor *bmd;
	IOMemoryMap *map;

	/* pageable backing: it claims the address range, its pages exist only if something touches an empty slot */
	bmd = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIOMemoryPageable | kIODirectionInOut,
	    (vm_size_t)size, PAGE_SIZE);
	if (bmd == nullptr)
		return nullptr;
	map = bmd->createMappingInTask(kernel_task, 0, kIOMapAnywhere, 0, size);
	if (map == nullptr) {
		bmd->release();
		return nullptr;
	}
	*kva = (void *)map->getVirtualAddress();
	return xg_vm_xnew(bmd, map);
}

extern "C" xg_vm_xmem_t
xg_vm_x_map_at(xg_vm_xmem_t resv, void *va, const unsigned long long *pa, unsigned int n, int cache)
{
	IOAddressRange *r;
	IOMemoryDescriptor *md;
	IOMemoryMap *map;
	unsigned int i, nr = 0;
	mach_vm_address_t base = resv->map->getAddress();

	if ((mach_vm_address_t)va < base || (mach_vm_address_t)va + ptoa_64(n) > base + resv->map->getLength())
		return nullptr;
	r = (IOAddressRange *)IOMalloc(sizeof(*r) * n);
	if (r == nullptr)
		return nullptr;
	for (i = 0; i < n; i++) {
		if (nr > 0 && r[nr - 1].address + r[nr - 1].length == pa[i]) {
			r[nr - 1].length += PAGE_SIZE;
			continue;
		}
		r[nr].address = pa[i];
		r[nr].length = PAGE_SIZE;
		nr++;
	}
	md = IOMemoryDescriptor::withAddressRanges(r, nr, kIODirectionInOut | kIOMemoryTypePhysical64 |
	    kIOMemoryMapperNone, nullptr);
	IOFree(r, sizeof(*r) * n);
	if (md == nullptr)
		return nullptr;
	/* Overwrite replaces the reservation's entries in place; such a map leaves its PTEs behind when released */
	map = md->createMappingInTask(kernel_task, (mach_vm_address_t)va, kIOMapOverwrite | xg_vm_cacheopt(cache), 0,
	    ptoa_64(n));
	if (map == nullptr) {
		md->release();
		return nullptr;
	}
	return xg_vm_xnew(md, map);
}

extern "C" int
xg_vm_x_unmap_at(xg_vm_xmem_t resv, void *va, unsigned long long size)
{
	IOMemoryMap *map;
	mach_vm_address_t base = resv->map->getAddress();

	if ((mach_vm_address_t)va < base || (mach_vm_address_t)va + size > base + resv->map->getLength())
		return -1;
	/* Unique: a fresh mapping of the reservation's backing, never its own map reused */
	map = resv->md->createMappingInTask(kernel_task, (mach_vm_address_t)va, kIOMapOverwrite | kIOMapUnique,
	    (mach_vm_address_t)va - base, size);
	if (map == nullptr)
		return -1;
	map->release();
	return 0;
}

extern "C" void
xg_vm_x_release(xg_vm_xmem_t xm)
{
	if (xm == nullptr)
		return;
	if (xm->map)
		xm->map->release();
	if (xm->md)
		xm->md->release();
	IOFree(xm, sizeof(*xm));
}

extern "C" unsigned long long
xg_vm_x_kextract(const void *va)
{
	IOMemoryDescriptor *md;
	IOByteCount len = 0;
	addr64_t pa = 0;
	mach_vm_address_t a = (mach_vm_address_t)va;

	md = IOMemoryDescriptor::withAddressRange(trunc_page_64(a), PAGE_SIZE, kIODirectionInOut, kernel_task);
	if (md == nullptr)
		return 0;
	if (md->prepare() == kIOReturnSuccess) {
		pa = md->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
		md->complete();
	}
	md->release();
	return pa != 0 ? pa + (a & PAGE_MASK) : 0;
}

extern "C" unsigned long long
xg_vm_x_memsize(void)
{
	return max_mem;
}

extern "C" void
xg_vm_x_flush(void *va, unsigned long long len)
{
	IOFlushProcessorCache(kernel_task, (IOVirtualAddress)va, (IOByteCount)len);
}
