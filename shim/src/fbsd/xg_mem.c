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
struct xg_hdr { size_t size; uint32_t magic; uint32_t pad; };
size_t malloc_usable_size(const void *addr);

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
	h = (flags & M_ZERO) ? IOMallocZero(size + XG_HDR) : IOMalloc(size + XG_HDR);
	if (h == NULL) {
		if (flags & M_WAITOK)
			panic("xegfx: malloc(%zu, %s) failed with M_WAITOK", size, type ? type->ks_shortdesc : "?");
		return (NULL);
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
	IOFree(h, h->size + XG_HDR);
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
