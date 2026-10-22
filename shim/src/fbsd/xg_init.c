/*
 * XeGfx: the kext's linker sets and start sequence. Runs the shim's init hooks in dependency order, then every
 * SYSINIT in the kext sorted by subsystem and order, as FreeBSD's mi_startup and the module loader do.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/linker_set.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <machine/stdarg.h>
#include "xg_xnu.h"

void xg_kern_init(void);
void xg_sched_init(void);
void xg_vm_init(void);
void xg_misc_init(void);
void xg_task_init(void);
void xg_pci_init(void);
void xg_dma_init(void);
void xg_newbus_dump(void);

extern const struct xg_set_entry xg_set_start __asm("section$start$__DATA$__xg_set");
extern const struct xg_set_entry xg_set_end __asm("section$end$__DATA$__xg_set");

const struct xg_set_entry *
xg_set_first(const char *set, const struct xg_set_entry *after)
{
	const struct xg_set_entry *e = after ? after + 1 : &xg_set_start;

	for (; e < &xg_set_end; e++)
		if (strcmp(e->set, set) == 0)
			return (e);
	return (NULL);
}

static int
xg_sysinit_cmp(const void *a, const void *b)
{
	const struct sysinit *x = *(const struct sysinit * const *)a, *y = *(const struct sysinit * const *)b;

	if (x->subsystem != y->subsystem)
		return (x->subsystem < y->subsystem ? -1 : 1);
	return (x->order < y->order ? -1 : x->order > y->order);
}

static int
xg_sysinit_is_gpu_driver(const struct sysinit *si)
{
	const moduledata_t *md;

	if (si->func != (sysinit_cfunc_t)module_register_init || si->udata == NULL)
		return (0);
	md = si->udata;
	return (md->name != NULL && (strcmp(md->name, "i915kms") == 0 || strcmp(md->name, "xe") == 0));
}

static void
xg_run_set(const char *set, int reverse)
{
	const struct xg_set_entry *e;
	struct sysinit **v;
	int n = 0, i;

	for (e = xg_set_first(set, NULL); e != NULL; e = xg_set_first(set, e))
		n++;
	if (n == 0)
		return;
	v = malloc(sizeof(*v) * n, M_TEMP, M_WAITOK);
	i = 0;
	for (e = xg_set_first(set, NULL); e != NULL; e = xg_set_first(set, e))
		v[i++] = (struct sysinit *)(uintptr_t)e->sym;
	qsort(v, n, sizeof(*v), xg_sysinit_cmp);
	/*
	 * On FreeBSD the GPU drivers are their own klds, loaded after drm.ko (MODULE_DEPEND), so all of drm's SYSINITs,
	 * drm_core_init at SI_SUB_OFED_MODINIT included, have run before i915kms registers at SI_SUB_DRIVERS. In one
	 * kext the sort would put i915 first (N-219: "DRM core is not initialized"), so their module records go last.
	 */
	for (int pass = 0; pass < 2; pass++) {
		for (i = 0; i < n; i++) {
			struct sysinit *si = v[reverse ? n - 1 - i : i];

			if (si->func == NULL || xg_sysinit_is_gpu_driver(si) != (pass == 1))
				continue;
			si->func(si->udata);
		}
	}
	printf("XeGfx: ran %d %s entries\n", n, set);
	free(v, M_TEMP);
}

/*
 * The start sequence in stages, so a boot can stop after any of them: 1 the shim's own state (clock, threads, VM,
 * tunables, queues), 3 the bus and DMA layers (newbus over every IOPCIDevice), 4 the drivers' SYSINITs (drm, i915
 * probe). Stage 2, a read-only look at the GPU, lives on the XNU side. Each stage runs once.
 */
void
xg_start_stage(int stage)
{
	switch (stage) {
	case 1:
		xg_kern_init();
		xg_sched_init();
		xg_vm_init();
		xg_misc_init();
		xg_task_init();
		break;
	case 3:
		xg_pci_init();
		xg_dma_init();
		break;
	case 4:
		xg_run_set("sysinit_set", 0);
		xg_newbus_dump();
		break;
	}
}

void
xg_start(void)
{
	xg_start_stage(1);
	xg_start_stage(3);
	xg_start_stage(4);
}

void
xg_stop(void)
{
	xg_run_set("sysuninit_set", 1);
}

void
log(int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
