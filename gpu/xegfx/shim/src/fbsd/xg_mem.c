/*
 * XeGfx: FreeBSD malloc(9), contigmalloc and UMA zones on XNU's IOMalloc family. FreeBSD's free() takes no size and
 * XNU's IOFree needs one, so every block carries a 16-byte header with its size.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <vm/vm.h>
#include <vm/uma.h>
#include <machine/atomic.h>
#include <machine/cpu.h>
#include "xg_xnu.h"

MALLOC_DEFINE(M_TEMP, "temp", "misc temporary data buffers");
MALLOC_DEFINE(M_DEVBUF, "devbuf", "device driver memory");

#define XG_HDR		16
#define XG_MAGIC	0x5867414cu	/* "XgAL" */
/* pool: 0 for an IOMalloc block, else 1 + the reserve class it came from */
struct xg_hdr { size_t size; uint32_t magic; uint32_t pool; };
size_t malloc_usable_size(const void *addr);

/*
 * M_NOWAIT reserve. Linux GFP_ATOMIC callers allocate under spinlocks (i915_active_add_request in execbuffer), and
 * IOMalloc may block for a zone refill there: N-237 panicked "blocking while holding a spinlock". So M_NOWAIT takes
 * a preallocated block of its size class under a spin word and never enters the allocator; a thread call tops the
 * classes up from a context that may sleep.
 */
#define XG_NW_CLASSES	7		/* 64 .. 4096 bytes including the header */
#define XG_NW_CAP	512
#define XG_NW_LOW	128
struct xg_nw_class {
	volatile u_int	lock;
	int		count;
	void		*items[XG_NW_CAP];
};
static struct xg_nw_class xg_nw[XG_NW_CLASSES];
static xg_call_t xg_nw_call;
static volatile u_int xg_nw_state;	/* 0 cold, 1 starting, 2 up */
static volatile u_int xg_nw_pending, xg_nw_misses;

static size_t
xg_nw_size(int c)
{
	return ((size_t)64 << c);
}

static int
xg_nw_class_of(size_t total)
{
	for (int c = 0; c < XG_NW_CLASSES; c++)
		if (total <= xg_nw_size(c))
			return (c);
	return (-1);
}

static void
xg_nw_lock(struct xg_nw_class *k)
{
	while (!atomic_cmpset_acq_int(&k->lock, 0, 1))
		cpu_spinwait();
}

static void
xg_nw_unlock(struct xg_nw_class *k)
{
	atomic_store_rel_int(&k->lock, 0);
}

static void
xg_nw_refill(void *p0, void *p1)
{
	for (int c = 0; c < XG_NW_CLASSES; c++) {
		struct xg_nw_class *k = &xg_nw[c];

		for (;;) {
			void *b;

			xg_nw_lock(k);
			if (k->count >= XG_NW_CAP / 2) {
				xg_nw_unlock(k);
				break;
			}
			xg_nw_unlock(k);
			if ((b = IOMalloc(xg_nw_size(c))) == NULL)
				break;
			xg_nw_lock(k);
			if (k->count < XG_NW_CAP) {
				k->items[k->count++] = b;
				b = NULL;
			}
			xg_nw_unlock(k);
			if (b != NULL)
				IOFree(b, xg_nw_size(c));
		}
	}
	atomic_store_rel_int(&xg_nw_pending, 0);
}

static void
xg_nw_kick(void)
{
	if (xg_nw_call != NULL && atomic_cmpset_int(&xg_nw_pending, 0, 1))
		thread_call_enter1_delayed(xg_nw_call, NULL, 0);
}

/* from the first M_WAITOK allocation: that context may sleep, so the reserve is filled there once */
static void
xg_nw_start(void)
{
	if (!atomic_cmpset_int(&xg_nw_state, 0, 1))
		return;
	xg_nw_call = thread_call_allocate_with_options(xg_nw_refill, NULL, XG_TC_PRIORITY_KERNEL, 0);
	xg_nw_refill(NULL, NULL);
	atomic_store_rel_int(&xg_nw_state, 2);
}

static struct xg_hdr *
xg_nw_take(size_t total)
{
	int c = xg_nw_class_of(total);
	struct xg_nw_class *k;
	struct xg_hdr *h = NULL;
	int left = 0;

	if (c < 0)
		return (NULL);
	k = &xg_nw[c];
	xg_nw_lock(k);
	if (k->count > 0) {
		h = k->items[--k->count];
		left = k->count;
	}
	xg_nw_unlock(k);
	if (h == NULL)
		atomic_add_int(&xg_nw_misses, 1);
	if (left < XG_NW_LOW)
		xg_nw_kick();
	if (h != NULL)
		h->pool = (uint32_t)c + 1;
	return (h);
}

/* true when the block went back to its class; a full class leaves it to the caller's IOFree */
static bool
xg_nw_give(struct xg_hdr *h)
{
	struct xg_nw_class *k = &xg_nw[h->pool - 1];
	bool kept = false;

	xg_nw_lock(k);
	if (k->count < XG_NW_CAP) {
		k->items[k->count++] = h;
		kept = true;
	}
	xg_nw_unlock(k);
	return (kept);
}

void
malloc_init(void *data)
{
}

void
malloc_uninit(void *data)
{
}

void *
(malloc)(size_t size, struct malloc_type *type, int flags)
{
	struct xg_hdr *h;

	if (size > SIZE_MAX - XG_HDR)
		return (NULL);
	if ((flags & M_NOWAIT) && atomic_load_acq_int(&xg_nw_state) == 2 &&
	    (h = xg_nw_take(size + XG_HDR)) != NULL) {
		if (flags & M_ZERO)
			memset(h, 0, size + XG_HDR);
		h->pool = (uint32_t)xg_nw_class_of(size + XG_HDR) + 1;
	} else {
		if ((flags & M_NOWAIT) == 0 && xg_nw_state == 0)
			xg_nw_start();
		/* still IOMalloc for M_NOWAIT before the reserve exists and above 4 KiB */
		h = (flags & M_ZERO) ? IOMallocZero(size + XG_HDR) : IOMalloc(size + XG_HDR);
		if (h == NULL) {
			if (flags & M_WAITOK)
				panic("xegfx: malloc(%zu, %s) failed with M_WAITOK", size, type ? type->ks_shortdesc : "?");
			return (NULL);
		}
		h->pool = 0;
	}
	h->size = size;
	h->magic = XG_MAGIC;
	return ((char *)h + XG_HDR);
}

/*
 * contigmalloc blocks are recorded by address: LinuxKPI's kmalloc hands every request above a page to contigmalloc
 * but frees it with plain free(), which FreeBSD resolves through its slab lookup (N-218 panicked in free() on a
 * 13.6 KiB drm device). Contig blocks are page-aligned and header blocks never are, so only aligned pointers look here.
 */
#define XG_CONTIG_BUCKETS 1024
struct xg_contig { void *addr; unsigned long size; struct xg_contig *next; };
static struct xg_contig *xg_contig_tab[XG_CONTIG_BUCKETS];
static volatile u_int xg_contig_lock;

static u_int
xg_contig_bucket(const void *p)
{
	return ((u_int)(((uintptr_t)p >> PAGE_SHIFT) & (XG_CONTIG_BUCKETS - 1)));
}

static void
xg_contig_lock_acquire(void)
{
	while (!atomic_cmpset_acq_int(&xg_contig_lock, 0, 1))
		cpu_spinwait();
}

static void
xg_contig_lock_release(void)
{
	atomic_store_rel_int(&xg_contig_lock, 0);
}

/* the block's size, 0 if p is not a contigmalloc block; remove drops the record */
static unsigned long
xg_contig_find(const void *p, bool remove)
{
	struct xg_contig **pp, *c;
	unsigned long size = 0;

	if (p == NULL || ((uintptr_t)p & PAGE_MASK) != 0)
		return (0);
	xg_contig_lock_acquire();
	for (pp = &xg_contig_tab[xg_contig_bucket(p)]; (c = *pp) != NULL; pp = &c->next) {
		if (c->addr == p) {
			size = c->size;
			if (remove)
				*pp = c->next;
			break;
		}
	}
	xg_contig_lock_release();
	if (remove && c != NULL)
		IOFree(c, sizeof(*c));
	return (size);
}

static struct xg_hdr *
xg_hdr_of(const void *p)
{
	struct xg_hdr *h = (struct xg_hdr *)((const char *)p - XG_HDR);

	if (h->magic != XG_MAGIC)
		panic("xegfx: free of %p, not a malloc block (magic 0x%x)", p, h->magic);
	return (h);
}

void
free(void *addr, struct malloc_type *type)
{
	struct xg_hdr *h;

	unsigned long csize;

	if (addr == NULL)
		return;
	if ((csize = xg_contig_find(addr, true)) != 0) {
		IOFreeContiguous(addr, csize);
		return;
	}
	h = xg_hdr_of(addr);
	h->magic = 0;
	if (h->pool != 0) {
		if (!xg_nw_give(h))
			IOFree(h, xg_nw_size((int)h->pool - 1));
		return;
	}
	IOFree(h, h->size + XG_HDR);
}

/* for debug.xegfx_mem: the reserve's fill per class and how often M_NOWAIT found it empty */
void
xg_mem_diag(char *buf, unsigned long len)
{
	int n = snprintf(buf, len, "nowait reserve");

	for (int c = 0; c < XG_NW_CLASSES && n > 0 && (unsigned long)n < len; c++)
		n += snprintf(buf + n, len - n, " %zu:%d", xg_nw_size(c), xg_nw[c].count);
	if (n > 0 && (unsigned long)n < len)
		snprintf(buf + n, len - n, ", misses %u", xg_nw_misses);
}

void
zfree(void *addr, struct malloc_type *type)
{
	if (addr != NULL)
		explicit_bzero(addr, malloc_usable_size(addr));
	free(addr, type);
}

size_t
malloc_usable_size(const void *addr)
{
	unsigned long csize = xg_contig_find(addr, false);

	return (addr == NULL ? 0 : csize != 0 ? csize : xg_hdr_of(addr)->size);
}

size_t
malloc_size(size_t size)
{
	return (size);
}

void *
realloc(void *addr, size_t size, struct malloc_type *type, int flags)
{
	void *n;
	size_t old;

	if (addr == NULL)
		return (malloc(size, type, flags));
	old = malloc_usable_size(addr);
	if (size <= old)
		return (addr);
	if ((n = malloc(size, type, flags)) == NULL)
		return (NULL);
	memcpy(n, addr, old);
	free(addr, type);
	return (n);
}

void *
mallocarray(size_t nmemb, size_t size, struct malloc_type *type, int flags)
{
	if (WOULD_OVERFLOW(nmemb, size))
		panic("xegfx: mallocarray(%zu, %zu) overflows", nmemb, size);
	return (malloc(nmemb * size, type, flags));
}

void *
malloc_domainset(size_t size, struct malloc_type *type, struct domainset *ds, int flags)
{
	return (malloc(size, type, flags));
}

char *
strdup(const char *s, struct malloc_type *type)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n, type, M_WAITOK);

	memcpy(d, s, n);
	return (d);
}

/* physically contiguous memory: XNU's allocator gives no physical range control, so a block outside
 * [low, high] or across the boundary is refused (logged) rather than handed out */
void *
contigmalloc(unsigned long size, struct malloc_type *type, int flags, vm_paddr_t low, vm_paddr_t high,
    unsigned long alignment, vm_paddr_t boundary)
{
	unsigned long long pa = 0;
	void *p;

	size = round_page(size);
	p = IOMallocContiguous(size, MAX(alignment, PAGE_SIZE), &pa);
	if (p == NULL)
		return (NULL);
	if (pa < low || pa + size - 1 > high ||
	    (boundary != 0 && (pa & ~(boundary - 1)) != ((pa + size - 1) & ~(boundary - 1)))) {
		printf("xegfx: contigmalloc(%lu) got 0x%llx, outside [0x%lx, 0x%lx] or boundary 0x%lx\n",
		    size, pa, (u_long)low, (u_long)high, (u_long)boundary);
		IOFreeContiguous(p, size);
		return (NULL);
	}
	if (flags & M_ZERO)
		memset(p, 0, size);
	{
		struct xg_contig *c = IOMalloc(sizeof(*c));
		u_int b = xg_contig_bucket(p);

		if (c == NULL) {
			IOFreeContiguous(p, size);
			return (NULL);
		}
		c->addr = p;
		c->size = size;
		xg_contig_lock_acquire();
		c->next = xg_contig_tab[b];
		xg_contig_tab[b] = c;
		xg_contig_lock_release();
	}
	return (p);
}

void *
contigmalloc_domainset(unsigned long size, struct malloc_type *type, struct domainset *ds, int flags,
    vm_paddr_t low, vm_paddr_t high, unsigned long alignment, vm_paddr_t boundary)
{
	return (contigmalloc(size, type, flags, low, high, alignment, boundary));
}

void
contigfree(void *addr, unsigned long size, struct malloc_type *type)
{
	unsigned long csize = xg_contig_find(addr, true);

	if (addr != NULL)
		IOFreeContiguous(addr, csize != 0 ? csize : round_page(size));
}

/* UMA zones without caching: each item is a malloc block, ctor/dtor and init/fini run on every alloc/free */
struct uma_zone {
	const char *name;
	size_t size;
	uma_ctor ctor;
	uma_dtor dtor;
	uma_init init;
	uma_fini fini;
};

static MALLOC_DEFINE(M_XGUMA, "xguma", "UMA zone items");

unsigned int
uma_get_cache_align_mask(void)
{
	return (CACHE_LINE_SIZE - 1);
}

uma_zone_t
uma_zcreate(const char *name, size_t size, uma_ctor ctor, uma_dtor dtor, uma_init uminit, uma_fini fini,
    int align, uint32_t flags)
{
	uma_zone_t z = malloc(sizeof(*z), M_XGUMA, M_WAITOK | M_ZERO);

	z->name = name;
	z->size = size;
	z->ctor = ctor;
	z->dtor = dtor;
	z->init = uminit;
	z->fini = fini;
	return (z);
}

void
uma_zdestroy(uma_zone_t zone)
{
	free(zone, M_XGUMA);
}

void *
uma_zalloc_arg(uma_zone_t zone, void *arg, int flags)
{
	void *item = malloc(zone->size, M_XGUMA, flags);

	if (item == NULL)
		return (NULL);
	if (zone->init != NULL && zone->init(item, (int)zone->size, flags) != 0)
		goto fail;
	if (zone->ctor != NULL && zone->ctor(item, (int)zone->size, arg, flags) != 0) {
		if (zone->fini != NULL)
			zone->fini(item, (int)zone->size);
		goto fail;
	}
	return (item);
fail:
	free(item, M_XGUMA);
	return (NULL);
}

void
uma_zfree_arg(uma_zone_t zone, void *item, void *arg)
{
	if (item == NULL)
		return;
	if (zone->dtor != NULL)
		zone->dtor(item, (int)zone->size, arg);
	if (zone->fini != NULL)
		zone->fini(item, (int)zone->size);
	free(item, M_XGUMA);
}

void
uma_prealloc(uma_zone_t zone, int itemcnt)
{
}

void
uma_zone_reserve(uma_zone_t zone, int nitems)
{
}
