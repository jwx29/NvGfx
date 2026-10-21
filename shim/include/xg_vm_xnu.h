/*
 * XeGfx vm: the IOKit half of the FreeBSD VM shim (shim/src/xnu/xg_vm.cpp), with plain C types so both the FreeBSD
 * side and the XNU side can include it. Every handle owns IOKit objects and is freed with xg_vm_x_release().
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#ifndef _XG_VM_XNU_H_
#define _XG_VM_XNU_H_
#ifdef __cplusplus
extern "C" {
#endif

typedef struct xg_vm_xmem *xg_vm_xmem_t;

/* cache modes, translated from VM_MEMATTR_* on the FreeBSD side */
#define	XG_VM_CACHE_DEFAULT	0	/* IOKit default: WB for RAM, UC for device memory */
#define	XG_VM_CACHE_WB		1
#define	XG_VM_CACHE_WC		2
#define	XG_VM_CACHE_UC		3
#define	XG_VM_CACHE_WT		4

/* wired, physically contiguous RAM below physmask (its low zero bits give the alignment) */
xg_vm_xmem_t xg_vm_x_alloc(unsigned long long size, unsigned long long physmask, int cache, void **kva,
    unsigned long long *pa);
/* kernel mapping of a physical range (device memory or RAM) */
xg_vm_xmem_t xg_vm_x_map_phys(unsigned long long pa, unsigned long long size, int cache, void **kva);
/* kernel VA reserved with no-access guard pages, filled later by xg_vm_x_map_at() */
xg_vm_xmem_t xg_vm_x_reserve(unsigned long long size, void **kva);
/* map n pages (physical addresses pa[]) over va, which lies inside the reservation resv */
xg_vm_xmem_t xg_vm_x_map_at(xg_vm_xmem_t resv, void *va, const unsigned long long *pa, unsigned int n, int cache);
/* put guard pages back over [va, va + size) inside the reservation */
int xg_vm_x_unmap_at(xg_vm_xmem_t resv, void *va, unsigned long long size);
void xg_vm_x_release(xg_vm_xmem_t xm);

/* physical address behind a wired kernel VA, 0 when none; may block (it wires the page for the lookup) */
unsigned long long xg_vm_x_kextract(const void *va);
/* XNU's direct map of all RAM (physmap), the FreeBSD DMAP on this kernel */
void xg_vm_x_dmap(unsigned long long *base, unsigned long long *limit);
unsigned long long xg_vm_x_memsize(void);
/* write back and invalidate CPU caches for a kernel range */
void xg_vm_x_flush(void *va, unsigned long long len);

#ifdef __cplusplus
}
#endif
#endif
