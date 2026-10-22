/*
 * XeGfx: FreeBSD VM pages, objects, pmap and kernel VA for drm-kmod and LinuxKPI on XNU (design: docs/xg_vm.md).
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/sx.h>
#include <sys/proc.h>
#include <sys/refcount.h>
#include <sys/sleepqueue.h>
#include <sys/sglist.h>
#include <sys/fbio.h>
#include <machine/atomic.h>
#include <machine/cpufunc.h>

/* pctrie's lock-free (SMR) lookups are not used on XNU; the locked API below is FreeBSD's own code */
#define	_SYS_SMR_H_
#include <sys/_smr.h>
#include <sys/smr_types.h>
void xg_unimpl(const char *name) __dead2;
static inline void smr_enter(smr_t smr) { xg_unimpl("smr_enter (lock-free pctrie lookup)"); }
static inline void smr_exit(smr_t smr) { }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wconditional-uninitialized"
#include <kern/subr_pctrie.c>
#pragma clang diagnostic pop

/* machine/pmap.h inlines (invlpg, PCID) read per-CPU fields the shim has no use for; they are never called here */
#define	XG_PCPU_pcid_invlpg_workaround()	0
#define	XG_PCPU_zpcpu_offset()			0
#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_pageout.h>
#include <vm/vm_phys.h>
#include <vm/vm_radix.h>
#include <vm/swap_pager.h>
#include <vm/uma.h>
#include <dev/vt/vt.h>
#include "xg_vm_xnu.h"
#include "xg_vmap.h"

static MALLOC_DEFINE(M_XGVM, "xgvm", "XeGfx VM shim");

/* ---- globals FreeBSD and LinuxKPI read directly ---- */
struct kva_layout_s kva_layout;
long physmem;
struct vm_object kernel_object_store;
struct proc *pageproc;		/* no page daemon: curproc is never pageproc */

static const struct pagerops xg_vm_nullpager;
/* no pager methods: inline helpers such as vm_pager_page_unswapped find NULL and do nothing */
const struct pagerops *pagertab[16] = { [0 ... 15] = &xg_vm_nullpager };

/* no vt(4): an empty console device whose framebuffer drm_os_freebsd may freeze and unfreeze */
static struct fb_info xg_vm_fb;
static struct vt_device xg_vm_vd = { .vd_softc = &xg_vm_fb };
struct vt_device *main_vd = &xg_vm_vd;

/* XNU x86_64 kernel VA and user VA limits */
#define	XG_VM_KM_LOW	0xffffff8000000000ul
#define	XG_VM_KM_HIGH	0xfffffffffffff000ul
#define	XG_VM_MAXUSER	0x00007ffffffff000ul

/*
 * Physical page runs. Every page the shim hands out lives in a run: one IOKit allocation (or a registered fictitious
 * range) with its struct vm_page array, so m + i is the next physical page as FreeBSD callers expect. Runs sit in a
 * pctrie keyed by physical address; PHYS_TO_VM_PAGE is a less-or-equal lookup.
 */
#define	XG_RUN_PAGES	1	/* vm_page_alloc*: CPU access through the DMAP */
#define	XG_RUN_KMEM	2	/* kmem_alloc_contig: also has a kernel mapping */
#define	XG_RUN_FICT	3	/* vm_phys_fictitious_reg_range: device memory, no backing */

struct xg_vm_run {
	uint64_t	pa;		/* trie key */
	u_long		npages;
	volatile u_long	nlive;		/* pages not freed yet */
	int		kind;
	xg_vm_xmem_t	xm;
	char		*kva;		/* the IOKit allocation's kernel mapping */
	struct vm_page	pages[];
};

/* kernel VA the shim made: kmem_alloc_contig, pmap_mapdev_attr and kva_alloc ranges, keyed by address */
#define	XG_KM_KMEM	1
#define	XG_KM_DEV	2
#define	XG_KM_KVA	3
#define	XG_KM_PAGES	4	/* a page run's mapping: what PHYS_TO_DMAP returns for its pages */
#define	XG_KM_PMAP	5	/* a kept one-page mapping of a physical page the shim did not allocate */

struct xg_vm_sub {			/* one pmap_qenter mapping inside a kva_alloc range */
	struct xg_vm_sub *next;
	vm_offset_t	va;
	int		npages;
	xg_vm_xmem_t	xm;
};

struct xg_vm_kmap {
	uint64_t	kva;		/* trie key */
	vm_size_t	size;
	int		kind;
	vm_paddr_t	pa;		/* KMEM, DEV */
	xg_vm_xmem_t	xm;		/* allocation, mapping or reservation */
	struct xg_vm_run *run;		/* KMEM */
	vm_paddr_t	*pas;		/* KVA: page entered in each slot, 0 when empty */
	struct xg_vm_sub *subs;		/* KVA */
};

static struct mtx xg_vm_mtx;		/* both tries and the kva sub lists */
static struct pctrie xg_vm_runs;
static struct pctrie xg_vm_kmaps;
static struct sx xg_vm_devsx;		/* device pager objects by handle */
static TAILQ_HEAD(, vm_object) xg_vm_devobjs = TAILQ_HEAD_INITIALIZER(xg_vm_devobjs);
static volatile u_long xg_vm_npages;	/* pages the shim holds, for vm_free_count */
static volatile u_int xg_vm_state;	/* 0 cold, 1 initialising, 2 up */

static void *
xg_vm_node_alloc(struct pctrie *ptree)
{
	void *n = malloc(pctrie_node_size(), M_XGVM, M_NOWAIT);

	if (n != NULL)
		pctrie_zone_init(n, 0, 0);
	return (n);
}

static void
xg_vm_node_free(struct pctrie *ptree, void *node)
{
	free(node, M_XGVM);
}

PCTRIE_DEFINE(XG_VM_RUN, xg_vm_run, pa, xg_vm_node_alloc, xg_vm_node_free);
PCTRIE_DEFINE(XG_VM_KMAP, xg_vm_kmap, kva, xg_vm_node_alloc, xg_vm_node_free);

/* physical page -> kept kernel mapping, for PHYS_TO_DMAP of pages outside the shim's runs */
struct xg_vm_pmap {
	uint64_t	pa;		/* trie key, page aligned */
	void		*kva;
	xg_vm_xmem_t	xm;
};
PCTRIE_DEFINE(XG_VM_PMAP, xg_vm_pmap, pa, xg_vm_node_alloc, xg_vm_node_free);
static struct pctrie xg_vm_pmaps;
static int xg_vm_kmap_insert(struct xg_vm_kmap *k);
static struct xg_vm_kmap *xg_vm_kmap_find_locked(vm_offset_t va);
static struct xg_vm_kmap *xg_vm_kmap_take(vm_offset_t kva, int kind);
void linux_free_pages(struct vm_page *page, unsigned int order);	/* LinuxKPI's struct page is vm_page */

/* object page tries (vm_radix.h) */
void *
vm_radix_node_alloc(struct pctrie *ptree)
{
	return (xg_vm_node_alloc(ptree));
}

void
vm_radix_node_free(struct pctrie *ptree, void *node)
{
	xg_vm_node_free(ptree, node);
}

static void
xg_vm_object_init(vm_object_t obj, objtype_t type, vm_pindex_t size)
{
	rw_init(&obj->lock, "vmobject");
	vm_radix_init(&obj->rtree);
	LIST_INIT(&obj->shadow_head);
	obj->type = type;
	obj->size = size;
	obj->ref_count = 1;
	obj->memattr = VM_MEMATTR_DEFAULT;
	switch (type) {
	case OBJT_DEVICE:
	case OBJT_SG:
		obj->flags = OBJ_FICTITIOUS | OBJ_UNMANAGED;
		break;
	case OBJT_MGTDEVICE:
		obj->flags = OBJ_FICTITIOUS;
		break;
	case OBJT_PHYS:
		obj->flags = OBJ_UNMANAGED;
		break;
	case OBJT_SWAP:
		obj->flags = OBJ_SWAP;
		break;
	default:
		obj->flags = 0;
		break;
	}
}

static void
xg_vm_setup(void)
{
	mtx_init(&xg_vm_mtx, "xgvm", NULL, MTX_DEF);
	sx_init(&xg_vm_devsx, "xgvmdev");
	pctrie_init(&xg_vm_runs);
	pctrie_init(&xg_vm_kmaps);
	pctrie_init(&xg_vm_pmaps);
	/* no direct map: PHYS_TO_DMAP and friends are lookups (shim machine/vmparam.h, xg_vm_phys_to_kva) */
	kva_layout.dmap_low = 0;
	kva_layout.dmap_high = 0;
	kva_layout.km_low = XG_VM_KM_LOW;
	kva_layout.km_high = XG_VM_KM_HIGH;
	kva_layout.kva_min = XG_VM_KM_LOW;
	kva_layout.kva_max = XG_VM_KM_HIGH;
	physmem = (long)atop(xg_vm_x_memsize());
	xg_vm_object_init(&kernel_object_store, OBJT_PHYS, atop(XG_VM_KM_HIGH - XG_VM_KM_LOW));
}

/* idempotent; the kext start calls it, every entry point below checks it */
void
xg_vm_init(void)
{
	if (atomic_load_acq_int(&xg_vm_state) == 2)
		return;
	if (atomic_cmpset_int(&xg_vm_state, 0, 1)) {
		xg_vm_setup();
		atomic_store_rel_int(&xg_vm_state, 2);
		return;
	}
	while (atomic_load_acq_int(&xg_vm_state) != 2)
		cpu_spinwait();
}

static int
xg_vm_cache(vm_memattr_t ma)
{
	switch (ma) {
	case VM_MEMATTR_WRITE_COMBINING:
		return (XG_VM_CACHE_WC);
	case VM_MEMATTR_UNCACHEABLE:
	case VM_MEMATTR_WEAK_UNCACHEABLE:
		return (XG_VM_CACHE_UC);
	case VM_MEMATTR_WRITE_THROUGH:
		return (XG_VM_CACHE_WT);
	case VM_MEMATTR_WRITE_BACK:
		return (XG_VM_CACHE_WB);
	default:
		return (XG_VM_CACHE_DEFAULT);
	}
}

/* ---- busy lock and wiring (vm_page.c semantics, no page queues: nothing is ever paged out) ---- */
static void
xg_vm_wakeup(void *chan)
{
	sleepq_lock(chan);
	sleepq_broadcast(chan, SLEEPQ_SLEEP, 0, 0);
	sleepq_release(chan);
}

static bool
xg_vm_tryxbusy(vm_page_t m)
{
	return (atomic_cmpset_acq_int(&m->busy_lock, VPB_UNBUSIED, VPB_CURTHREAD_EXCLUSIVE) != 0);
}

static bool
xg_vm_trysbusy(vm_page_t m)
{
	u_int x = atomic_load_int(&m->busy_lock);

	for (;;) {
		if ((x & VPB_BIT_SHARED) == 0)
			return (false);
		if (atomic_fcmpset_acq_int(&m->busy_lock, &x, x + VPB_ONE_SHARER))
			return (true);
	}
}

static bool
xg_vm_trybusy(vm_page_t m, int allocflags)
{
	if ((allocflags & (VM_ALLOC_SBUSY | VM_ALLOC_IGN_SBUSY)) != 0)
		return (xg_vm_trysbusy(m));
	return (xg_vm_tryxbusy(m));
}

static void
xg_vm_sunbusy(vm_page_t m)
{
	u_int x = atomic_load_int(&m->busy_lock);

	for (;;) {
		if (VPB_SHARERS(x) > 1) {
			if (atomic_fcmpset_int(&m->busy_lock, &x, x - VPB_ONE_SHARER))
				return;
			continue;
		}
		if (atomic_fcmpset_rel_int(&m->busy_lock, &x, VPB_UNBUSIED)) {
			if ((x & VPB_BIT_WAITERS) != 0)
				xg_vm_wakeup(m);
			return;
		}
	}
}

static void
xg_vm_busy_release(vm_page_t m)
{
	if ((atomic_load_int(&m->busy_lock) & VPB_BIT_SHARED) != 0)
		xg_vm_sunbusy(m);
	else
		vm_page_xunbusy(m);
}

void
vm_page_xunbusy_hard(vm_page_t m)
{
	atomic_store_rel_int(&m->busy_lock, VPB_UNBUSIED);
	xg_vm_wakeup(m);
}

static void
xg_vm_wire(vm_page_t m)
{
	atomic_add_int(&m->ref_count, 1);
}

/* _vm_page_busy_sleep: true when it slept (and dropped the object lock if locked) */
static bool
xg_vm_busy_sleep(vm_object_t obj, vm_page_t m, vm_pindex_t pindex, int allocflags, bool locked)
{
	bool xsleep = (allocflags & (VM_ALLOC_SBUSY | VM_ALLOC_IGN_SBUSY)) != 0;
	u_int x;

	if (atomic_load_int(&m->busy_lock) == VPB_UNBUSIED)
		return (false);
	sleepq_lock(m);
	x = atomic_load_int(&m->busy_lock);
	do {
		if (x == VPB_UNBUSIED || (xsleep && (x & VPB_BIT_SHARED) != 0) ||
		    m->object != obj || m->pindex != pindex) {
			sleepq_release(m);
			return (false);
		}
		if ((x & VPB_BIT_WAITERS) != 0)
			break;
	} while (!atomic_fcmpset_int(&m->busy_lock, &x, x | VPB_BIT_WAITERS));
	if (locked)
		VM_OBJECT_WUNLOCK(obj);
	sleepq_add(m, NULL, "vmpba", 0, 0);
	sleepq_wait(m, PVM);
	return (true);
}

bool
vm_page_busy_acquire(vm_page_t m, int allocflags)
{
	vm_object_t obj = atomic_load_ptr(&m->object);
	bool locked;

	for (;;) {
		if (xg_vm_trybusy(m, allocflags)) {
			if ((allocflags & VM_ALLOC_WIRED) != 0)
				xg_vm_wire(m);
			return (true);
		}
		if ((allocflags & VM_ALLOC_NOWAIT) != 0)
			return (false);
		/* VM_OBJECT_WOWNED without _rw_wowned, which the lock shim does not export */
		locked = obj != NULL && !(obj->lock.rw_lock & RW_LOCK_READ) &&
		    RW_OWNER(obj->lock.rw_lock) == (uintptr_t)curthread;
		if (xg_vm_busy_sleep(obj, m, m->pindex, allocflags, locked) && locked)
			VM_OBJECT_WLOCK(obj);
		if ((allocflags & VM_ALLOC_WAITFAIL) != 0)
			return (false);
	}
}

/* ---- page runs ---- */
static void
xg_vm_page_init(vm_page_t m, vm_paddr_t pa, vm_memattr_t ma)
{
	m->phys_addr = pa;
	m->a.queue = PQ_NONE;
	m->oflags = VPO_UNMANAGED;
	m->busy_lock = VPB_UNBUSIED;
	TAILQ_INIT(&m->md.pv_list);
	m->md.pat_mode = ma;
}

static struct xg_vm_run *
xg_vm_run_new(u_long npages, int kind)
{
	struct xg_vm_run *r;

	r = malloc(sizeof(*r) + npages * sizeof(struct vm_page), M_XGVM, M_NOWAIT | M_ZERO);
	if (r != NULL) {
		r->npages = npages;
		r->nlive = npages;
		r->kind = kind;
	}
	return (r);
}

static int
xg_vm_run_insert(struct xg_vm_run *r)
{
	struct xg_vm_run *prev;
	int error;

	mtx_lock(&xg_vm_mtx);
	prev = XG_VM_RUN_PCTRIE_LOOKUP_LE(&xg_vm_runs, r->pa + ptoa(r->npages) - 1);
	if (prev != NULL && prev->pa + ptoa(prev->npages) > r->pa)
		error = EBUSY;
	else
		error = XG_VM_RUN_PCTRIE_INSERT(&xg_vm_runs, r);
	mtx_unlock(&xg_vm_mtx);
	return (error);
}

static struct xg_vm_run *
xg_vm_run_find(vm_paddr_t pa)
{
	struct xg_vm_run *r;

	xg_vm_init();
	mtx_lock(&xg_vm_mtx);
	r = XG_VM_RUN_PCTRIE_LOOKUP_LE(&xg_vm_runs, pa);
	if (r != NULL && pa >= r->pa + ptoa(r->npages))
		r = NULL;
	mtx_unlock(&xg_vm_mtx);
	return (r);
}

static void
xg_vm_run_destroy(struct xg_vm_run *r)
{
	struct xg_vm_kmap *k;

	if (r->kind == XG_RUN_PAGES && (k = xg_vm_kmap_take((vm_offset_t)r->kva, XG_KM_PAGES)) != NULL)
		free(k, M_XGVM);
	mtx_lock(&xg_vm_mtx);
	XG_VM_RUN_PCTRIE_REMOVE(&xg_vm_runs, r->pa);
	mtx_unlock(&xg_vm_mtx);
	if (r->kind != XG_RUN_FICT)
		atomic_subtract_long(&xg_vm_npages, r->npages);
	xg_vm_x_release(r->xm);
	free(r, M_XGVM);
}

/* physmask for IOBufferMemoryDescriptor: the address bits allowed under high, low bits cleared for alignment */
static uint64_t
xg_vm_physmask(vm_paddr_t high, u_long alignment)
{
	uint64_t mask;

	/* the largest 2^n - 1 not above high; the exact limit is checked after the allocation */
	if (high == ~(vm_paddr_t)0 || ((high + 1) & high) == 0)
		mask = high;
	else
		mask = (1ull << (flsll(high + 1) - 1)) - 1;
	if (alignment > PAGE_SIZE)
		mask &= ~((uint64_t)alignment - 1);
	return (mask & ~(uint64_t)PAGE_MASK);
}

/* one contiguous IOKit allocation, checked against FreeBSD's low/high/boundary limits */
static struct xg_vm_run *
xg_vm_run_alloc(u_long npages, vm_paddr_t low, vm_paddr_t high, u_long alignment, vm_paddr_t boundary,
    vm_memattr_t ma, int kind, void **kvap)
{
	struct xg_vm_run *r;
	struct xg_vm_kmap *pk = NULL;
	unsigned long long pa;
	void *kva;
	vm_size_t size = ptoa(npages);
	u_long i;

	xg_vm_init();
	if (npages == 0 || (alignment & (alignment - 1)) != 0 || (boundary & (boundary - 1)) != 0)
		return (NULL);
	if ((r = xg_vm_run_new(npages, kind)) == NULL)
		return (NULL);
	r->xm = xg_vm_x_alloc(size, xg_vm_physmask(high, alignment), kind == XG_RUN_KMEM ? xg_vm_cache(ma) :
	    XG_VM_CACHE_DEFAULT, &kva, &pa);
	if (r->xm == NULL)
		goto fail;
	if (pa < low || pa + size - 1 > high || (alignment > 1 && (pa & (alignment - 1)) != 0) ||
	    (boundary != 0 && (pa & ~(boundary - 1)) != ((pa + size - 1) & ~(boundary - 1)))) {
		printf("xegfx: vm: %lu pages at 0x%llx miss [0x%lx, 0x%lx] align 0x%lx boundary 0x%lx\n", npages,
		    pa, (u_long)low, (u_long)high, alignment, (u_long)boundary);
		goto fail;
	}
	r->pa = pa;
	r->kva = kva;
	/* page runs take their attribute after zeroing (pmap_page_set_memattr), kmem runs are mapped with it */
	for (i = 0; i < npages; i++)
		xg_vm_page_init(&r->pages[i], pa + ptoa(i), kind == XG_RUN_KMEM ? ma : VM_MEMATTR_DEFAULT);
	if (kind == XG_RUN_PAGES) {
		/* the run's mapping, so DMAP_TO_PHYS and VIRT_IN_DMAP find its pages' addresses */
		if ((pk = malloc(sizeof(*pk), M_XGVM, M_NOWAIT | M_ZERO)) == NULL)
			goto fail;
		pk->kva = (uint64_t)kva;
		pk->size = size;
		pk->kind = XG_KM_PAGES;
		pk->pa = pa;
		pk->run = r;
	}
	if (xg_vm_run_insert(r) != 0)
		goto fail;
	if (pk != NULL && xg_vm_kmap_insert(pk) != 0) {
		mtx_lock(&xg_vm_mtx);
		XG_VM_RUN_PCTRIE_REMOVE(&xg_vm_runs, r->pa);
		mtx_unlock(&xg_vm_mtx);
		goto fail;
	}
	atomic_add_long(&xg_vm_npages, npages);
	if (kvap != NULL)
		*kvap = kva;
	return (r);
fail:
	free(pk, M_XGVM);
	xg_vm_x_release(r->xm);
	free(r, M_XGVM);
	return (NULL);
}

static void
xg_vm_page_release(vm_page_t m)
{
	struct xg_vm_run *r = xg_vm_run_find(m->phys_addr);

	if (r == NULL)
		panic("xegfx: vm: free of page %p (pa 0x%lx) the shim does not own", m, (u_long)m->phys_addr);
	if (r->kind == XG_RUN_FICT)
		return;
	if (atomic_fetchadd_long(&r->nlive, -1) == 1)
		xg_vm_run_destroy(r);
}

static void
xg_vm_zero(vm_page_t m)
{
	memset(PHYS_TO_DMAP(VM_PAGE_TO_PHYS(m)), 0, PAGE_SIZE);
}

vm_page_t
PHYS_TO_VM_PAGE(vm_paddr_t pa)
{
	struct xg_vm_run *r = xg_vm_run_find(pa);

	return (r == NULL ? NULL : &r->pages[atop(pa - r->pa)]);
}

vm_page_t
vm_page_alloc_noobj_contig_domain(int domain, int req, u_long npages, vm_paddr_t low, vm_paddr_t high,
    u_long alignment, vm_paddr_t boundary, vm_memattr_t memattr)
{
	struct xg_vm_run *r;
	u_long i;

	for (;;) {
		r = xg_vm_run_alloc(npages, low, high, MAX(alignment, PAGE_SIZE), boundary, memattr, XG_RUN_PAGES,
		    NULL);
		if (r != NULL)
			break;
		if ((req & (VM_ALLOC_WAITOK | VM_ALLOC_WAITFAIL)) == 0)
			return (NULL);
		vm_wait(NULL);
		if ((req & VM_ALLOC_WAITFAIL) != 0)
			return (NULL);
	}
	for (i = 0; i < npages; i++) {
		vm_page_t m = &r->pages[i];

		if ((req & VM_ALLOC_WIRED) != 0)
			m->ref_count = 1;
		if ((req & VM_ALLOC_NOFREE) != 0)
			m->flags |= PG_NOFREE;
		if ((req & VM_ALLOC_ZERO) != 0) {
			xg_vm_zero(m);
			m->flags |= PG_ZERO;
		}
		if (memattr != VM_MEMATTR_DEFAULT)
			pmap_page_set_memattr(m, memattr);
	}
	return (&r->pages[0]);
}

vm_page_t
vm_page_alloc_noobj_contig(int req, u_long npages, vm_paddr_t low, vm_paddr_t high, u_long alignment,
    vm_paddr_t boundary, vm_memattr_t memattr)
{
	return (vm_page_alloc_noobj_contig_domain(0, req, npages, low, high, alignment, boundary, memattr));
}

vm_page_t
vm_page_alloc_noobj(int req)
{
	return (vm_page_alloc_noobj_contig_domain(0, req, 1, 0, ~(vm_paddr_t)0, PAGE_SIZE, 0,
	    VM_MEMATTR_DEFAULT));
}

/* nothing the shim could evict to make a contiguous run: report a shortage so callers vm_wait and retry */
int
vm_page_reclaim_contig(int req, u_long npages, vm_paddr_t low, vm_paddr_t high, u_long alignment,
    vm_paddr_t boundary)
{
	return (ENOMEM);
}

/* XNU frees memory on its own schedule; give it a tick */
void
vm_wait(vm_object_t obj)
{
	pause("vmwait", hz / 100 + 1);
}

/* XNU's free count is not in the KPI: report what the shim does not hold of half of RAM */
u_int
vm_free_count(void)
{
	long half = physmem / 2, held = (long)xg_vm_npages;

	return (held >= half ? 0 : (u_int)MIN(half - held, UINT_MAX));
}

void
swap_pager_status(int *total, int *used)
{
	*total = 0;
	*used = 0;
}

/* ---- fictitious pages ---- */
void
vm_page_initfake(vm_page_t m, vm_paddr_t paddr, vm_memattr_t memattr)
{
	if ((m->flags & PG_FICTITIOUS) == 0) {
		m->phys_addr = paddr;
		m->a.queue = PQ_NONE;
		m->flags = PG_FICTITIOUS;
		m->oflags = VPO_UNMANAGED;
		m->busy_lock = VPB_CURTHREAD_EXCLUSIVE;
		m->ref_count = 1;
		TAILQ_INIT(&m->md.pv_list);
	}
	pmap_page_set_memattr(m, memattr);
}

vm_page_t
vm_page_getfake(vm_paddr_t paddr, vm_memattr_t memattr)
{
	vm_page_t m = malloc(sizeof(*m), M_XGVM, M_WAITOK | M_ZERO);

	vm_page_initfake(m, paddr, memattr);
	return (m);
}

void
vm_page_updatefake(vm_page_t m, vm_paddr_t paddr, vm_memattr_t memattr)
{
	m->phys_addr = paddr;
	pmap_page_set_memattr(m, memattr);
}

void
vm_page_putfake(vm_page_t m)
{
	if (m->object != NULL)
		atomic_swap_int(&m->busy_lock, VPB_FREED);
	free(m, M_XGVM);
}

int
vm_phys_fictitious_reg_range(vm_paddr_t start, vm_paddr_t end, vm_memattr_t memattr)
{
	struct xg_vm_run *r;
	u_long i, n;

	xg_vm_init();
	if (start >= end || (start & PAGE_MASK) != 0)
		return (EINVAL);
	n = atop(end - start);
	r = malloc(sizeof(*r) + n * sizeof(struct vm_page), M_XGVM, M_WAITOK | M_ZERO);
	r->pa = start;
	r->npages = n;
	r->nlive = n;
	r->kind = XG_RUN_FICT;
	for (i = 0; i < n; i++) {
		vm_page_initfake(&r->pages[i], start + ptoa(i), memattr);
		r->pages[i].oflags &= ~VPO_UNMANAGED;
		r->pages[i].busy_lock = VPB_UNBUSIED;
	}
	if (xg_vm_run_insert(r) != 0) {
		free(r, M_XGVM);
		return (EBUSY);
	}
	return (0);
}

void
vm_phys_fictitious_unreg_range(vm_paddr_t start, vm_paddr_t end)
{
	struct xg_vm_run *r = xg_vm_run_find(start);

	if (r == NULL || r->kind != XG_RUN_FICT || r->pa != start || r->pa + ptoa(r->npages) != end) {
		printf("xegfx: vm: unregister of unknown fictitious range 0x%lx-0x%lx\n", (u_long)start, (u_long)end);
		return;
	}
	xg_vm_run_destroy(r);
}

/* ---- page state ---- */
void
vm_page_dirty_KBI(vm_page_t m)
{
	m->dirty = VM_PAGE_BITS_ALL;
}

void
vm_page_reference(vm_page_t m)
{
	vm_page_aflag_set(m, PGA_REFERENCED);
}

void
vm_page_valid(vm_page_t m)
{
	if (vm_page_xbusied(m))
		m->valid = VM_PAGE_BITS_ALL;
	else
		atomic_set_8((volatile uint8_t *)&m->valid, VM_PAGE_BITS_ALL);
}

/* ---- pages in objects ---- */
static void
xg_vm_insert_done(vm_page_t m, vm_object_t object)
{
	object->resident_page_count++;
	if ((object->flags & OBJ_UNMANAGED) == 0 && (m->flags & PG_FICTITIOUS) == 0)
		m->oflags &= ~VPO_UNMANAGED;
}

static void
xg_vm_remove_done(vm_page_t m)
{
	m->object->resident_page_count--;
	m->object = NULL;
}

int
vm_page_iter_insert(vm_page_t m, vm_object_t object, vm_pindex_t pindex, struct pctrie_iter *pages)
{
	m->object = object;
	m->pindex = pindex;
	m->ref_count |= VPRC_OBJREF;
	if (vm_radix_iter_insert(pages, m) != 0) {
		m->object = NULL;
		m->pindex = 0;
		m->ref_count &= ~VPRC_OBJREF;
		return (1);
	}
	xg_vm_insert_done(m, object);
	return (0);
}

static int
xg_vm_insert(vm_page_t m, vm_object_t object, vm_pindex_t pindex)
{
	m->object = object;
	m->pindex = pindex;
	m->ref_count |= VPRC_OBJREF;
	if (vm_radix_insert(&object->rtree, m) != 0) {
		m->object = NULL;
		m->pindex = 0;
		m->ref_count &= ~VPRC_OBJREF;
		return (1);
	}
	xg_vm_insert_done(m, object);
	return (0);
}

/* the caller holds the page xbusy; true when the object's reference was the page's last */
static bool
xg_vm_remove_xbusy(vm_page_t m)
{
	vm_radix_remove(&m->object->rtree, m->pindex);
	xg_vm_remove_done(m);
	return (vm_page_drop(m, VPRC_OBJREF) == VPRC_OBJREF);
}

bool
vm_page_remove(vm_page_t m)
{
	bool dropped = xg_vm_remove_xbusy(m);

	vm_page_xunbusy(m);
	return (dropped);
}

bool
vm_page_iter_remove(struct pctrie_iter *pages, vm_page_t m)
{
	bool dropped;

	vm_radix_iter_remove(pages);
	xg_vm_remove_done(m);
	dropped = vm_page_drop(m, VPRC_OBJREF) == VPRC_OBJREF;
	vm_page_xunbusy(m);
	return (dropped);
}

void
vm_page_replace(vm_page_t mnew, vm_object_t object, vm_pindex_t pindex, vm_page_t mold)
{
	bool dropped;

	mnew->object = object;
	mnew->pindex = pindex;
	atomic_set_int(&mnew->ref_count, VPRC_OBJREF);
	if (vm_radix_replace(&object->rtree, mnew) != mold)
		panic("xegfx: vm: vm_page_replace: %p was not at index %lu", mold, (u_long)pindex);
	mold->object = NULL;
	dropped = vm_page_drop(mold, VPRC_OBJREF) == VPRC_OBJREF;
	vm_page_xunbusy(mold);
	if (dropped)
		vm_page_free(mold);
}

void
vm_page_free(vm_page_t m)
{
	m->flags &= ~PG_ZERO;
	if (m->object != NULL) {
		VM_OBJECT_ASSERT_WLOCKED(m->object);
		vm_radix_remove(&m->object->rtree, m->pindex);
		xg_vm_remove_done(m);
		m->ref_count &= ~VPRC_OBJREF;
	}
	/* fictitious pages are never freed (vm_page_free_prep) */
	if ((m->flags & PG_FICTITIOUS) != 0) {
		if (m->busy_lock != VPB_UNBUSIED)
			vm_page_xunbusy(m);
		return;
	}
	if (VPRC_WIRE_COUNT(m->ref_count) != 0)
		panic("xegfx: vm: freeing wired page %p (ref 0x%x)", m, m->ref_count);
	if ((m->flags & PG_NOFREE) != 0)
		panic("xegfx: vm: freeing NOFREE page %p", m);
	if ((atomic_swap_int(&m->busy_lock, VPB_FREED) & VPB_BIT_WAITERS) != 0)
		xg_vm_wakeup(m);
	m->a.flags = 0;
	m->valid = 0;
	m->dirty = 0;
	xg_vm_page_release(m);
}

bool
vm_page_unwire_noq(vm_page_t m)
{
	u_int old = vm_page_drop(m, 1);

	if (VPRC_WIRE_COUNT(old) == 0)
		panic("xegfx: vm: unwire of unwired page %p", m);
	return (VPRC_WIRE_COUNT(old) == 1);
}

/* no page queues: the last wiring of an object page leaves it in the object, the last reference frees it */
void
vm_page_unwire(vm_page_t m, uint8_t queue)
{
	u_int old = vm_page_drop(m, 1);

	if (VPRC_WIRE_COUNT(old) == 0)
		panic("xegfx: vm: unwire of unwired page %p", m);
	if ((old & ~VPRC_BLOCKED) == 1)
		vm_page_free(m);
}

/* a new zero-filled page at pindex; the object is write locked, WAITFAIL may drop it to wait */
static vm_page_t
xg_vm_alloc_obj(vm_object_t object, vm_pindex_t pindex, int req, struct pctrie_iter *pages)
{
	vm_page_t m;

	m = vm_page_alloc_noobj_contig_domain(0, req & (VM_ALLOC_ZERO | VM_ALLOC_NOFREE), 1, 0,
	    ~(vm_paddr_t)0, PAGE_SIZE, 0, object->memattr);
	if (m == NULL) {
		if ((req & VM_ALLOC_WAITFAIL) != 0) {
			VM_OBJECT_WUNLOCK(object);
			vm_wait(object);
			VM_OBJECT_WLOCK(object);
			if (pages != NULL)
				pctrie_iter_reset(pages);
		}
		return (NULL);
	}
	m->busy_lock = VPB_CURTHREAD_EXCLUSIVE;
	if ((pages != NULL ? vm_page_iter_insert(m, object, pindex, pages) : xg_vm_insert(m, object, pindex)) !=
	    0) {
		m->busy_lock = VPB_UNBUSIED;
		vm_page_free(m);
		return (NULL);
	}
	if ((req & VM_ALLOC_WIRED) != 0)
		xg_vm_wire(m);
	if ((req & VM_ALLOC_SBUSY) != 0)
		m->busy_lock = VPB_SHARERS_WORD(1);
	return (m);
}

static int
xg_vm_grab_pflags(int allocflags)
{
	int pflags = allocflags & ~(VM_ALLOC_NOWAIT | VM_ALLOC_WAITOK | VM_ALLOC_WAITFAIL | VM_ALLOC_NOBUSY |
	    VM_ALLOC_IGN_SBUSY | VM_ALLOC_NOCREAT);

	if ((allocflags & VM_ALLOC_NOWAIT) == 0)
		pflags |= VM_ALLOC_WAITFAIL;
	if ((allocflags & VM_ALLOC_IGN_SBUSY) != 0)
		pflags |= VM_ALLOC_SBUSY;
	return (pflags);
}

/* vm_page_grab_sleep: true when the caller should look the page up again */
static bool
xg_vm_grab_sleep(vm_object_t object, vm_page_t m, vm_pindex_t pindex, int allocflags)
{
	if ((allocflags & VM_ALLOC_NOWAIT) != 0)
		return (false);
	if ((allocflags & VM_ALLOC_NOCREAT) == 0)
		vm_page_reference(m);
	if (xg_vm_busy_sleep(object, m, pindex, allocflags, true))
		VM_OBJECT_WLOCK(object);
	return ((allocflags & VM_ALLOC_WAITFAIL) == 0);
}

vm_page_t
vm_page_grab_iter(vm_object_t object, vm_pindex_t pindex, int allocflags, struct pctrie_iter *pages)
{
	vm_page_t m;

	VM_OBJECT_ASSERT_WLOCKED(object);
	for (;;) {
		m = vm_radix_iter_lookup(pages, pindex);
		if (m != NULL) {
			if (xg_vm_trybusy(m, allocflags)) {
				if ((allocflags & VM_ALLOC_WIRED) != 0)
					xg_vm_wire(m);
				break;
			}
			if (!xg_vm_grab_sleep(object, m, pindex, allocflags))
				return (NULL);
			pctrie_iter_reset(pages);
			continue;
		}
		if ((allocflags & VM_ALLOC_NOCREAT) != 0)
			return (NULL);
		m = xg_vm_alloc_obj(object, pindex, xg_vm_grab_pflags(allocflags), pages);
		if (m != NULL) {
			if ((allocflags & VM_ALLOC_ZERO) != 0 && (m->flags & PG_ZERO) == 0)
				pmap_zero_page(m);
			break;
		}
		if ((allocflags & (VM_ALLOC_NOWAIT | VM_ALLOC_WAITFAIL)) != 0)
			return (NULL);
	}
	if ((allocflags & VM_ALLOC_NOBUSY) != 0) {
		if ((allocflags & VM_ALLOC_IGN_SBUSY) != 0)
			xg_vm_sunbusy(m);
		else
			vm_page_xunbusy(m);
	}
	return (m);
}

/* swap objects have no backing store here, so a missing page is a zero-filled one (vm_page_zero_invalid) */
int
vm_page_grab_valid(vm_page_t *mp, vm_object_t object, vm_pindex_t pindex, int allocflags)
{
	struct pctrie_iter pages;
	vm_page_t m;
	int pflags;

	VM_OBJECT_ASSERT_WLOCKED(object);
	vm_page_iter_init(&pages, object);
	pflags = (allocflags & ~(VM_ALLOC_NOBUSY | VM_ALLOC_SBUSY | VM_ALLOC_WIRED | VM_ALLOC_IGN_SBUSY)) |
	    VM_ALLOC_WAITFAIL;
	for (;;) {
		m = vm_radix_iter_lookup(&pages, pindex);
		if (m != NULL) {
			if (!xg_vm_trybusy(m, vm_page_all_valid(m) ? allocflags : 0)) {
				(void)xg_vm_grab_sleep(object, m, pindex, allocflags);
				pctrie_iter_reset(&pages);
				continue;
			}
			if (vm_page_all_valid(m))
				goto out;
			if ((allocflags & VM_ALLOC_NOCREAT) != 0) {
				xg_vm_busy_release(m);
				*mp = NULL;
				return (VM_PAGER_FAIL);
			}
			break;
		}
		if ((allocflags & VM_ALLOC_NOCREAT) != 0) {
			*mp = NULL;
			return (VM_PAGER_FAIL);
		}
		if ((m = xg_vm_alloc_obj(object, pindex, pflags, &pages)) != NULL)
			break;
	}
	if ((m->flags & PG_ZERO) == 0)
		pmap_zero_page(m);
	m->valid = VM_PAGE_BITS_ALL;
out:
	if ((allocflags & VM_ALLOC_WIRED) != 0)
		xg_vm_wire(m);
	if ((allocflags & VM_ALLOC_SBUSY) != 0 && vm_page_xbusied(m))
		atomic_store_rel_int(&m->busy_lock, VPB_SHARERS_WORD(1));
	else if ((allocflags & VM_ALLOC_NOBUSY) != 0)
		xg_vm_busy_release(m);
	*mp = m;
	return (VM_PAGER_OK);
}

void
vm_page_iter_init(struct pctrie_iter *pages, vm_object_t object)
{
	vm_radix_iter_init(pages, &object->rtree);
}

void
vm_page_iter_limit_init(struct pctrie_iter *pages, vm_object_t object, vm_pindex_t limit)
{
	vm_radix_iter_limit_init(pages, &object->rtree, limit);
}

/* ---- objects ---- */
static vm_object_t
xg_vm_object_new(objtype_t type, vm_pindex_t size, void *handle)
{
	vm_object_t obj = malloc(sizeof(*obj), M_XGVM, M_WAITOK | M_ZERO);

	xg_vm_object_init(obj, type, size);
	obj->handle = handle;
	return (obj);
}

/* drop every page; wired pages lose the object's reference and are freed by their last unwire */
static void
xg_vm_object_free_pages(vm_object_t obj)
{
	struct pctrie_iter pages;
	vm_page_t m;

	VM_OBJECT_WLOCK(obj);
	vm_page_iter_init(&pages, obj);
restart:
	VM_RADIX_FOREACH(m, &pages) {
		if (!vm_page_busy_acquire(m, VM_ALLOC_WAITFAIL)) {
			pctrie_iter_reset(&pages);
			goto restart;
		}
		if (vm_page_iter_remove(&pages, m))
			vm_page_free(m);
		else if ((m->flags & PG_FICTITIOUS) != 0 && xg_vm_run_find(m->phys_addr) == NULL)
			vm_page_putfake(m);	/* a vm_page_getfake page (dev_pager_free_page) */
	}
	VM_OBJECT_WUNLOCK(obj);
}

static void
xg_vm_object_destroy(vm_object_t obj)
{
	if ((obj->type == OBJT_DEVICE || obj->type == OBJT_MGTDEVICE) && obj->un_pager.devp.ops != NULL)
		obj->un_pager.devp.ops->cdev_pg_dtor(obj->un_pager.devp.handle);
	xg_vm_object_free_pages(obj);
	if (obj->type == OBJT_SG && obj->handle != NULL)
		sglist_free(obj->handle);
	rw_destroy(&obj->lock);
	free(obj, M_XGVM);
}

void
vm_object_reference(vm_object_t obj)
{
	if (obj != NULL)
		refcount_acquire(&obj->ref_count);
}

void
vm_object_deallocate(vm_object_t obj)
{
	bool dev, last;

	if (obj == NULL || obj == kernel_object)
		return;
	dev = obj->type == OBJT_DEVICE || obj->type == OBJT_MGTDEVICE;
	/* device objects leave the handle list under the same lock lookups take, so none revives at zero */
	if (dev)
		sx_xlock(&xg_vm_devsx);
	last = refcount_release(&obj->ref_count);
	if (dev && last && obj->un_pager.devp.handle != NULL)
		TAILQ_REMOVE(&xg_vm_devobjs, obj, pager_object_list);
	if (dev)
		sx_xunlock(&xg_vm_devsx);
	if (last)
		xg_vm_object_destroy(obj);
}

int
vm_object_set_memattr(vm_object_t object, vm_memattr_t memattr)
{
	VM_OBJECT_ASSERT_WLOCKED(object);
	if (object->type == OBJT_DEAD)
		return (KERN_INVALID_ARGUMENT);
	if (!vm_radix_is_empty(&object->rtree))
		return (KERN_FAILURE);
	object->memattr = memattr;
	return (KERN_SUCCESS);
}

/* vm_object.c semantics: wired pages stay but lose their contents unless CLEANONLY, dirty pages stay with it */
void
vm_object_page_remove(vm_object_t object, vm_pindex_t start, vm_pindex_t end, int options)
{
	struct pctrie_iter pages;
	vm_page_t p;

	VM_OBJECT_ASSERT_WLOCKED(object);
	if (object->resident_page_count == 0)
		return;
	vm_page_iter_limit_init(&pages, object, end);
again:
	pctrie_iter_reset(&pages);
	for (p = vm_radix_iter_lookup_ge(&pages, start); p != NULL; p = vm_radix_iter_step(&pages)) {
		if (!xg_vm_tryxbusy(p)) {
			if (xg_vm_busy_sleep(object, p, p->pindex, 0, true))
				VM_OBJECT_WLOCK(object);
			goto again;
		}
		if ((options & OBJPR_VALIDONLY) != 0 && vm_page_none_valid(p)) {
			vm_page_xunbusy(p);
			continue;
		}
		if (vm_page_wired(p)) {
			if ((options & OBJPR_CLEANONLY) == 0) {
				p->valid = 0;
				p->dirty = 0;
			}
			vm_page_xunbusy(p);
			continue;
		}
		if ((options & OBJPR_CLEANONLY) != 0 && !vm_page_none_valid(p) && p->dirty != 0) {
			vm_page_xunbusy(p);
			continue;
		}
		if (vm_page_iter_remove(&pages, p)) {
			if ((p->flags & PG_FICTITIOUS) != 0 && xg_vm_run_find(p->phys_addr) == NULL)
				vm_page_putfake(p);
			else
				vm_page_free(p);
		}
	}
}

vm_object_t
vm_pager_allocate(objtype_t type, void *handle, vm_ooffset_t size, vm_prot_t prot, vm_ooffset_t off,
    struct ucred *cred)
{
	xg_vm_init();
	switch (type) {
	case OBJT_SWAP:
	case OBJT_PHYS:
	case OBJT_SG:
		/* swap: anonymous, zero-filled on first touch; SG: the sglist is kept as the handle */
		return (xg_vm_object_new(type, OFF_TO_IDX(round_page(off + size)), handle));
	default:
		printf("xegfx: vm: vm_pager_allocate of object type %d is not supported\n", type);
		return (NULL);
	}
}

/* ---- device pager objects (device_pager.c) ---- */
static vm_object_t
xg_vm_dev_find(void *handle)
{
	vm_object_t obj;

	TAILQ_FOREACH(obj, &xg_vm_devobjs, pager_object_list)
		if (obj->handle == handle)
			return (obj);
	return (NULL);
}

vm_object_t
cdev_pager_lookup(void *handle)
{
	vm_object_t obj;

	xg_vm_init();
	sx_slock(&xg_vm_devsx);
	obj = xg_vm_dev_find(handle);
	if (obj != NULL)
		refcount_acquire(&obj->ref_count);
	sx_sunlock(&xg_vm_devsx);
	return (obj);
}

vm_object_t
cdev_pager_allocate(void *handle, enum obj_type tp, const struct cdev_pager_ops *ops, vm_ooffset_t size,
    vm_prot_t prot, vm_ooffset_t foff, struct ucred *cred)
{
	vm_object_t obj;
	vm_pindex_t pindex;
	u_short color;

	xg_vm_init();
	if (handle == NULL || (tp != OBJT_DEVICE && tp != OBJT_MGTDEVICE) || (foff & PAGE_MASK) != 0)
		return (NULL);
	size = round_page(size);
	pindex = OFF_TO_IDX(foff) + OFF_TO_IDX(size);
	if (pindex > OBJ_MAX_SIZE || pindex < OFF_TO_IDX(foff) || pindex < OFF_TO_IDX(size))
		return (NULL);
	/* creation holds the list lock across the constructor, so a racing allocate finds the finished object */
	sx_xlock(&xg_vm_devsx);
	obj = xg_vm_dev_find(handle);
	if (obj != NULL) {
		refcount_acquire(&obj->ref_count);
		if (pindex > obj->size)
			obj->size = pindex;
		sx_xunlock(&xg_vm_devsx);
		return (obj);
	}
	obj = xg_vm_object_new((objtype_t)tp, pindex, handle);
	obj->un_pager.devp.ops = ops;
	if (tp == OBJT_DEVICE)
		obj->flags |= OBJ_PG_DTOR;
	if (ops->cdev_pg_populate != NULL)
		obj->flags |= OBJ_POPULATE;
	if (ops->cdev_pg_ctor(handle, size, prot, foff, cred, &color) != 0) {
		sx_xunlock(&xg_vm_devsx);
		obj->un_pager.devp.ops = NULL;	/* no dtor for a failed ctor */
		obj->type = OBJT_DEAD;
		xg_vm_object_destroy(obj);
		return (NULL);
	}
	obj->flags |= OBJ_COLORED;
	obj->pg_color = color;
	obj->un_pager.devp.handle = handle;
	TAILQ_INSERT_TAIL(&xg_vm_devobjs, obj, pager_object_list);
	sx_xunlock(&xg_vm_devsx);
	return (obj);
}

/* ---- kernel VA ---- */
static int
xg_vm_kmap_insert(struct xg_vm_kmap *k)
{
	int error;

	mtx_lock(&xg_vm_mtx);
	error = XG_VM_KMAP_PCTRIE_INSERT(&xg_vm_kmaps, k);
	mtx_unlock(&xg_vm_mtx);
	return (error);
}

/* the kmap containing va; the caller holds xg_vm_mtx */
static struct xg_vm_kmap *
xg_vm_kmap_find_locked(vm_offset_t va)
{
	struct xg_vm_kmap *k = XG_VM_KMAP_PCTRIE_LOOKUP_LE(&xg_vm_kmaps, va);

	return (k != NULL && va < k->kva + k->size ? k : NULL);
}

static struct xg_vm_kmap *
xg_vm_kmap_take(vm_offset_t kva, int kind)
{
	struct xg_vm_kmap *k;

	mtx_lock(&xg_vm_mtx);
	k = XG_VM_KMAP_PCTRIE_LOOKUP(&xg_vm_kmaps, kva);
	if (k != NULL && k->kind == kind)
		XG_VM_KMAP_PCTRIE_REMOVE(&xg_vm_kmaps, kva);
	else
		k = NULL;
	mtx_unlock(&xg_vm_mtx);
	return (k);
}

void *
kmem_alloc_contig(vm_size_t size, int flags, vm_paddr_t low, vm_paddr_t high, u_long alignment,
    vm_paddr_t boundary, vm_memattr_t memattr)
{
	struct xg_vm_kmap *k;
	struct xg_vm_run *r;
	void *kva;

	size = round_page(size);
	if (size == 0 || (k = malloc(sizeof(*k), M_XGVM, M_NOWAIT | M_ZERO)) == NULL)
		return (NULL);
	r = xg_vm_run_alloc(atop(size), low, high, MAX(alignment, PAGE_SIZE), boundary, memattr, XG_RUN_KMEM,
	    &kva);
	if (r == NULL) {
		free(k, M_XGVM);
		return (NULL);
	}
	for (u_long i = 0; i < r->npages; i++)
		r->pages[i].ref_count = 1;	/* kernel memory is wired */
	k->kva = (uint64_t)kva;
	k->size = size;
	k->kind = XG_KM_KMEM;
	k->pa = r->pa;
	k->run = r;
	if (xg_vm_kmap_insert(k) != 0) {
		r->nlive = 1;
		xg_vm_run_destroy(r);
		free(k, M_XGVM);
		return (NULL);
	}
	if ((flags & M_ZERO) != 0)
		memset(kva, 0, size);
	return (kva);
}

void
kmem_free(void *addr, vm_size_t size)
{
	struct xg_vm_kmap *k;

	if (addr == NULL)
		return;
	if ((k = xg_vm_kmap_take((vm_offset_t)addr, XG_KM_KMEM)) == NULL) {
		/* linux_free_kmem sends page addresses here too: they sit in the kernel map range on XNU */
		if (xg_vm_virt_in_dmap((vm_offset_t)addr)) {
			linux_free_pages(PHYS_TO_VM_PAGE(pmap_kextract((vm_offset_t)addr)),
			    (unsigned int)(flsl((long)atop(round_page(size))) - 1));
			return;
		}
		panic("xegfx: vm: kmem_free(%p, %lu) of memory kmem_alloc_contig did not return", addr, (u_long)size);
	}
	k->run->nlive = 1;
	xg_vm_run_destroy(k->run);
	free(k, M_XGVM);
}

void *
pmap_mapdev_attr(vm_paddr_t pa, vm_size_t size, int mode)
{
	struct xg_vm_kmap *k;
	vm_paddr_t base = trunc_page(pa);
	vm_size_t len = round_page(pa + size) - base;
	void *kva;

	xg_vm_init();
	if ((k = malloc(sizeof(*k), M_XGVM, M_WAITOK | M_ZERO)) == NULL)
		return (NULL);
	k->xm = xg_vm_x_map_phys(base, len, xg_vm_cache((vm_memattr_t)mode), &kva);
	if (k->xm == NULL) {
		free(k, M_XGVM);
		return (NULL);
	}
	k->kva = (uint64_t)kva;
	k->size = len;
	k->kind = XG_KM_DEV;
	k->pa = base;
	if (xg_vm_kmap_insert(k) != 0) {
		xg_vm_x_release(k->xm);
		free(k, M_XGVM);
		return (NULL);
	}
	return ((char *)kva + (pa - base));
}

void
pmap_unmapdev(void *va, vm_size_t size)
{
	struct xg_vm_kmap *k = xg_vm_kmap_take(trunc_page((vm_offset_t)va), XG_KM_DEV);

	if (k == NULL) {
		printf("xegfx: vm: pmap_unmapdev(%p) of an unknown mapping\n", va);
		return;
	}
	xg_vm_x_release(k->xm);
	free(k, M_XGVM);
}

void *
kva_alloc(vm_size_t size)
{
	struct xg_vm_kmap *k;
	void *kva;

	xg_vm_init();
	size = round_page(size);
	if (size == 0 || (k = malloc(sizeof(*k), M_XGVM, M_NOWAIT | M_ZERO)) == NULL)
		return (NULL);
	k->pas = malloc(atop(size) * sizeof(vm_paddr_t), M_XGVM, M_NOWAIT | M_ZERO);
	if (k->pas == NULL || (k->xm = xg_vm_x_reserve(size, &kva)) == NULL)
		goto fail;
	k->kva = (uint64_t)kva;
	k->size = size;
	k->kind = XG_KM_KVA;
	if (xg_vm_kmap_insert(k) != 0) {
		xg_vm_x_release(k->xm);
		goto fail;
	}
	return (kva);
fail:
	free(k->pas, M_XGVM);
	free(k, M_XGVM);
	return (NULL);
}

/* release the qenter mappings that lie wholly in [va, va + n pages); overwritten PTEs need no unmapping */
static void
xg_vm_subs_drop(struct xg_vm_kmap *k, vm_offset_t va, int n)
{
	struct xg_vm_sub **pp, *s, *dead = NULL;

	mtx_lock(&xg_vm_mtx);
	for (pp = &k->subs; (s = *pp) != NULL;) {
		if (s->va >= va && s->va + ptoa(s->npages) <= va + ptoa(n)) {
			*pp = s->next;
			s->next = dead;
			dead = s;
		} else
			pp = &s->next;
	}
	mtx_unlock(&xg_vm_mtx);
	while ((s = dead) != NULL) {
		dead = s->next;
		xg_vm_x_release(s->xm);
		free(s, M_XGVM);
	}
}

void
kva_free(void *addr, vm_size_t size)
{
	struct xg_vm_kmap *k;

	if (addr == NULL)
		return;
	if ((k = xg_vm_kmap_take((vm_offset_t)addr, XG_KM_KVA)) == NULL)
		panic("xegfx: vm: kva_free(%p) of an address kva_alloc did not return", addr);
	xg_vm_subs_drop(k, k->kva, atop(k->size));
	xg_vm_x_release(k->xm);		/* unmaps the whole range, entered pages included */
	free(k->pas, M_XGVM);
	free(k, M_XGVM);
}

static struct xg_vm_kmap *
xg_vm_kva_range(void *va, int count, const char *who)
{
	struct xg_vm_kmap *k;

	mtx_lock(&xg_vm_mtx);
	k = xg_vm_kmap_find_locked((vm_offset_t)va);
	mtx_unlock(&xg_vm_mtx);
	if (k == NULL || k->kind != XG_KM_KVA || (vm_offset_t)va + ptoa(count) > k->kva + k->size)
		panic("xegfx: vm: %s(%p, %d) outside any kva_alloc range", who, va, count);
	return (k);
}

/*
 * vmap() without kva_alloc: the pages go to an address IOKit picks. Overlaying a kva_alloc reservation with
 * kIOMapOverwrite fails on Tahoe (N-221: pmap_qenter panicked on i915's first vmap, its scratch page).
 */
struct xg_vm_vmap {
	LIST_ENTRY(xg_vm_vmap)	link;
	void			*va;
	xg_vm_xmem_t		xm;
};
static LIST_HEAD(, xg_vm_vmap) xg_vm_vmaps = LIST_HEAD_INITIALIZER(xg_vm_vmaps);

void *
xg_vm_vmap_pa(const unsigned long long *pa, unsigned int n, int memattr)
{
	unsigned long long *rp, *rl;
	struct xg_vm_vmap *v;
	unsigned int i, nr = 0;
	void *va = NULL;

	if (n == 0)
		return (NULL);
	rp = malloc(n * sizeof(*rp), M_XGVM, M_WAITOK);
	rl = malloc(n * sizeof(*rl), M_XGVM, M_WAITOK);
	for (i = 0; i < n; i++) {
		if (nr > 0 && rp[nr - 1] + rl[nr - 1] == pa[i]) {
			rl[nr - 1] += PAGE_SIZE;
			continue;
		}
		rp[nr] = pa[i];
		rl[nr++] = PAGE_SIZE;
	}
	v = malloc(sizeof(*v), M_XGVM, M_WAITOK | M_ZERO);
	v->xm = xg_vm_x_map_kernel(rp, rl, nr, xg_vm_cache((vm_memattr_t)memattr), &va);
	free(rp, M_XGVM);
	free(rl, M_XGVM);
	if (v->xm == NULL) {
		free(v, M_XGVM);
		return (NULL);
	}
	v->va = va;
	mtx_lock(&xg_vm_mtx);
	LIST_INSERT_HEAD(&xg_vm_vmaps, v, link);
	mtx_unlock(&xg_vm_mtx);
	return (va);
}

int
xg_vm_vunmap(void *va)
{
	struct xg_vm_vmap *v;

	mtx_lock(&xg_vm_mtx);
	LIST_FOREACH(v, &xg_vm_vmaps, link)
		if (v->va == va) {
			LIST_REMOVE(v, link);
			break;
		}
	mtx_unlock(&xg_vm_mtx);
	if (v == NULL)
		return (-1);
	xg_vm_x_release(v->xm);
	free(v, M_XGVM);
	return (0);
}

/* stage 1 runs this before any GPU work: 0, or the negative step that failed (no panic) */
int
xg_vm_vmap_selftest(void)
{
	unsigned long long pa[2];
	volatile uint32_t *a, *b;
	int rc = 0, i;

	a = contigmalloc(2 * PAGE_SIZE, M_XGVM, M_WAITOK | M_ZERO, 0, ~(vm_paddr_t)0, PAGE_SIZE, 0);
	if (a == NULL)
		return (-1);
	for (i = 0; i < 2; i++)
		pa[i] = pmap_kextract((vm_offset_t)a + ptoa(i));
	if (pa[0] == 0 || pa[1] == 0)
		rc = -2;
	else if ((b = xg_vm_vmap_pa(pa, 2, VM_MEMATTR_WRITE_BACK)) == NULL)
		rc = -3;
	else {
		b[0] = 0x58670001;
		b[PAGE_SIZE / 4 + 5] = 0x58670002;
		if (a[0] != 0x58670001 || a[PAGE_SIZE / 4 + 5] != 0x58670002)
			rc = -4;
		if (xg_vm_vunmap((void *)(uintptr_t)b) != 0)
			rc = rc ? rc : -5;
	}
	contigfree((void *)(uintptr_t)a, 2 * PAGE_SIZE, M_XGVM);
	printf("xegfx: vm: vmap selftest %d\n", rc);
	return (rc);
}

/* maps each group of pages with the same memattr in one IOKit mapping over the reserved VA; may block */
void
pmap_qenter(void *va, vm_page_t *ma, int count)
{
	struct xg_vm_kmap *k;
	struct xg_vm_sub *s;
	unsigned long long *pa;
	vm_offset_t base = (vm_offset_t)va;
	int i, j;

	if (count <= 0)
		return;
	k = xg_vm_kva_range(va, count, "pmap_qenter");
	pa = malloc(count * sizeof(*pa), M_XGVM, M_WAITOK);
	for (i = 0; i < count; i++)
		pa[i] = VM_PAGE_TO_PHYS(ma[i]);
	xg_vm_subs_drop(k, base, count);
	for (i = 0; i < count; i = j) {
		for (j = i + 1; j < count && ma[j]->md.pat_mode == ma[i]->md.pat_mode; j++)
			;
		s = malloc(sizeof(*s), M_XGVM, M_WAITOK | M_ZERO);
		s->va = base + ptoa(i);
		s->npages = j - i;
		s->xm = xg_vm_x_map_at(k->xm, (void *)s->va, &pa[i], j - i, xg_vm_cache((vm_memattr_t)ma[i]->md.pat_mode));
		if (s->xm == NULL)
			panic("xegfx: vm: pmap_qenter could not map %d pages at %p", j - i, (void *)s->va);
		mtx_lock(&xg_vm_mtx);
		memcpy(&k->pas[atop(s->va - k->kva)], &pa[i], (j - i) * sizeof(vm_paddr_t));
		s->next = k->subs;
		k->subs = s;
		mtx_unlock(&xg_vm_mtx);
	}
	free(pa, M_XGVM);
}

void
pmap_qremove(void *va, int count)
{
	struct xg_vm_kmap *k;

	if (count <= 0)
		return;
	k = xg_vm_kva_range(va, count, "pmap_qremove");
	if (xg_vm_x_unmap_at(k->xm, va, ptoa(count)) != 0)
		panic("xegfx: vm: pmap_qremove could not unmap %d pages at %p", count, va);
	mtx_lock(&xg_vm_mtx);
	memset(&k->pas[atop((vm_offset_t)va - k->kva)], 0, count * sizeof(vm_paddr_t));
	mtx_unlock(&xg_vm_mtx);
	xg_vm_subs_drop(k, (vm_offset_t)va, count);
}

/* the DMAP is arithmetic, the shim's own ranges are looked up, anything else asks IOKit (which may block) */
vm_paddr_t
pmap_kextract(vm_offset_t va)
{
	struct xg_vm_kmap *k;
	vm_paddr_t pa = 0;
	bool found = false;

	xg_vm_init();
	mtx_lock(&xg_vm_mtx);
	if ((k = xg_vm_kmap_find_locked(va)) != NULL) {
		found = true;
		if (k->kind == XG_KM_KVA)
			pa = k->pas[atop(va - k->kva)] != 0 ? k->pas[atop(va - k->kva)] + (va & PAGE_MASK) : 0;
		else
			pa = k->pa + (va - k->kva);
	}
	mtx_unlock(&xg_vm_mtx);
	return (found ? pa : (vm_paddr_t)xg_vm_x_kextract((const void *)va));
}

/* ---- pmap page operations ---- */
void
pmap_zero_page(vm_page_t m)
{
	xg_vm_zero(m);
}

void
pmap_copy_page(vm_page_t src, vm_page_t dst)
{
	memcpy(PHYS_TO_DMAP(VM_PAGE_TO_PHYS(dst)), PHYS_TO_DMAP(VM_PAGE_TO_PHYS(src)), PAGE_SIZE);
}

/*
 * The attribute applies to later mappings (pmap_qenter, user maps). the page's own kernel mapping stays write-back, so moving
 * a RAM page to WC or UC writes its lines back first, as FreeBSD's pmap does when it changes the DMAP.
 */
void
pmap_page_set_memattr(vm_page_t m, vm_memattr_t ma)
{
	vm_memattr_t old = (vm_memattr_t)m->md.pat_mode;

	m->md.pat_mode = ma;
	if (old != ma && ma != VM_MEMATTR_WRITE_BACK && (m->flags & PG_FICTITIOUS) == 0)
		xg_vm_x_flush(PHYS_TO_DMAP(VM_PAGE_TO_PHYS(m)), PAGE_SIZE);
}

/* no managed (user) mappings exist until GEM mmap lands (docs/xg_vm.md) */
bool
pmap_page_is_mapped(vm_page_t m)
{
	return (false);
}

void
pmap_remove_all(vm_page_t m)
{
}

/* wbinvd reaches this CPU only; every Xe-era CPU has clflush, so drm takes this path only as a fallback */
void
pmap_invalidate_cache(void)
{
	static int warned;

	if (!warned) {
		warned = 1;
		printf("xegfx: vm: pmap_invalidate_cache flushes the current CPU only\n");
	}
	wbinvd();
}

/* ---- user memory ---- */
bool
vm_map_range_valid_KBI(vm_map_t map, vm_offset_t start, vm_offset_t end)
{
	return (start < end && end <= XG_VM_MAXUSER);
}

/* user page wiring (userptr, get_user_pages) is X5 work: until then every lookup misses (docs/xg_vm.md) */
vm_page_t
pmap_extract_and_hold(pmap_t pmap, vm_offset_t va, vm_prot_t prot)
{
	return (NULL);
}

int
vm_fault_quick_hold_pages(vm_map_t map, vm_offset_t addr, vm_size_t len, vm_prot_t prot, vm_page_t *ma,
    int max_count)
{
	static int warned;

	if (!warned) {
		warned = 1;
		printf("xegfx: vm: user page holds (userptr) are not implemented yet\n");
	}
	return (-1);
}

/* GEM mmap into a process needs IOMemoryDescriptor::createMappingInTask on the object's pages (docs/xg_vm.md) */
int
vm_map_find(vm_map_t map, vm_object_t object, vm_ooffset_t offset, vm_offset_t *addr, vm_size_t length,
    vm_offset_t max_addr, int find_space, vm_prot_t prot, vm_prot_t max, int cow)
{
	printf("xegfx: vm: vm_map_find (legacy GEM mmap) is not implemented yet\n");
	return (KERN_NO_SPACE);
}

int
vm_mmap_object(vm_map_t map, vm_offset_t *addr, vm_size_t size, vm_prot_t prot, vm_prot_t maxprot, int flags,
    vm_object_t object, vm_ooffset_t foff, boolean_t writecounted, struct thread *td)
{
	printf("xegfx: vm: vm_mmap_object (DRM mmap) is not implemented yet\n");
	return (ENODEV);
}

int
vm_mmap_to_errno(int rv)
{
	switch (rv) {
	case KERN_SUCCESS:
		return (0);
	case KERN_INVALID_ADDRESS:
	case KERN_NO_SPACE:
		return (ENOMEM);
	case KERN_PROTECTION_FAILURE:
		return (EACCES);
	default:
		return (EINVAL);
	}
}

int
vm_fault_disable_pagefaults(void)
{
	return (curthread_pflags_set(TDP_NOFAULTING | TDP_RESETSPUR));
}

void
vm_fault_enable_pagefaults(int save)
{
	curthread_pflags_restore(save);
}

/*
 * XNU's copyin resolves faults by sleeping; FreeBSD's _nofault variants may fail instead, so they fail inside a
 * critical section (where sleeping is not allowed) and otherwise copy normally.
 */
int
copyin_nofault(const void * __restrict udaddr, void * _Nonnull __restrict kaddr, size_t len)
{
	if (curthread->td_critnest != 0)
		return (EFAULT);
	return (copyin(udaddr, kaddr, len));
}

int
copyout_nofault(const void * _Nonnull __restrict kaddr, void * __restrict udaddr, size_t len)
{
	if (curthread->td_critnest != 0)
		return (EFAULT);
	return (copyout(kaddr, udaddr, len));
}

/* cache zones need uma_zalloc to call the import function, which xg_mem.c's zones do not do yet */
uma_zone_t
uma_zcache_create(const char *name, int size, uma_ctor ctor, uma_dtor dtor, uma_init zinit, uma_fini zfini,
    uma_import zimport, uma_release zrelease, void *arg, int flags)
{
	xg_unimpl("uma_zcache_create");
}

/* ---- the FreeBSD DMAP as lookups: XNU gives kexts no direct map of RAM ---- */

void *
xg_vm_phys_to_kva(vm_paddr_t pa)
{
	struct xg_vm_run *r = xg_vm_run_find(pa);
	struct xg_vm_pmap *p, *n;
	struct xg_vm_kmap *k;
	vm_paddr_t page = trunc_page(pa);
	void *kva;

	if (r != NULL && r->kva != NULL)
		return (r->kva + (pa - r->pa));
	mtx_lock(&xg_vm_mtx);
	p = XG_VM_PMAP_PCTRIE_LOOKUP(&xg_vm_pmaps, page);
	mtx_unlock(&xg_vm_mtx);
	if (p != NULL)
		return ((char *)p->kva + (pa & PAGE_MASK));
	/* first CPU access to a page the shim did not allocate: a one-page IOKit mapping, kept (this may block) */
	if (curthread->td_critnest != 0)
		printf("xegfx: vm: PHYS_TO_DMAP(0x%lx) maps a page inside a critical section\n", (u_long)pa);
	n = malloc(sizeof(*n), M_XGVM, M_WAITOK | M_ZERO);
	k = malloc(sizeof(*k), M_XGVM, M_WAITOK | M_ZERO);
	if ((n->xm = xg_vm_x_map_phys(page, PAGE_SIZE, XG_VM_CACHE_DEFAULT, &kva)) == NULL)
		panic("xegfx: vm: cannot map physical page 0x%lx", (u_long)page);
	n->pa = page;
	n->kva = kva;
	mtx_lock(&xg_vm_mtx);
	p = XG_VM_PMAP_PCTRIE_LOOKUP(&xg_vm_pmaps, page);
	if (p == NULL && XG_VM_PMAP_PCTRIE_INSERT(&xg_vm_pmaps, n) == 0) {
		p = n;
		n = NULL;
	}
	mtx_unlock(&xg_vm_mtx);
	if (n != NULL) {		/* raced with another mapper, or no trie node: use theirs or fail */
		xg_vm_x_release(n->xm);
		free(n, M_XGVM);
		free(k, M_XGVM);
		if (p == NULL)
			panic("xegfx: vm: no memory to record the mapping of 0x%lx", (u_long)page);
		return ((char *)p->kva + (pa & PAGE_MASK));
	}
	k->kva = (uint64_t)kva;
	k->size = PAGE_SIZE;
	k->kind = XG_KM_PMAP;
	k->pa = page;
	k->xm = NULL;		/* owned by the pmap entry */
	if (xg_vm_kmap_insert(k) != 0)
		free(k, M_XGVM);	/* the mapping still works; only DMAP_TO_PHYS falls back to IOKit */
	return ((char *)p->kva + (pa & PAGE_MASK));
}

vm_paddr_t
xg_vm_kva_to_phys(vm_offset_t va)
{
	return (pmap_kextract(va));
}

int
xg_vm_virt_in_dmap(vm_offset_t va)
{
	struct xg_vm_kmap *k;
	int in;

	mtx_lock(&xg_vm_mtx);
	k = xg_vm_kmap_find_locked(va);
	in = k != NULL && (k->kind == XG_KM_PAGES || k->kind == XG_KM_PMAP);
	mtx_unlock(&xg_vm_mtx);
	return (in);
}
