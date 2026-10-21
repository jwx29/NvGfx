/* XeGfx: amd64 vmparam.h without a direct map. XNU gives kexts no mapping of all RAM, so PHYS_TO_DMAP returns the
 * kernel address of the shim's page run (or a kept one-page IOKit mapping) and DMAP_TO_PHYS looks it up (xg_vm.c). */
#include_next <machine/vmparam.h>
#ifndef _XG_VMPARAM_H_
#define _XG_VMPARAM_H_
void *xg_vm_phys_to_kva(unsigned long pa);
unsigned long xg_vm_kva_to_phys(unsigned long va);
int xg_vm_virt_in_dmap(unsigned long va);
#undef PHYS_TO_DMAP_ADDR
#undef PHYS_TO_DMAP
#undef DMAP_TO_PHYS
#undef PHYS_IN_DMAP
#undef VIRT_IN_DMAP
#define	PHYS_TO_DMAP_ADDR(x)	((unsigned long)xg_vm_phys_to_kva((unsigned long)(x)))
#define	PHYS_TO_DMAP(x)		xg_vm_phys_to_kva((unsigned long)(x))
#define	DMAP_TO_PHYS(x)		xg_vm_kva_to_phys((unsigned long)(x))
#define	PHYS_IN_DMAP(pa)	1
#define	VIRT_IN_DMAP(va)	xg_vm_virt_in_dmap((unsigned long)(va))
#endif
