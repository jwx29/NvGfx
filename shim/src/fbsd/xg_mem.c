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
#include "xg_xnu.h"

MALLOC_DEFINE(M_TEMP, "temp", "misc temporary data buffers");
MALLOC_DEFINE(M_DEVBUF, "devbuf", "device driver memory");

#define XG_HDR		16
#define XG_MAGIC	0x5867414cu	/* "XgAL" */
struct xg_hdr { size_t size; uint32_t magic; uint32_t pad; };

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

	if (addr == NULL)
		return;
	h = xg_hdr_of(addr);
	h->magic = 0;
	IOFree(h, h->size + XG_HDR);
}

void
zfree(void *addr, struct malloc_type *type)
{
	if (addr != NULL)
		explicit_bzero(addr, xg_hdr_of(addr)->size);
	free(addr, type);
}

size_t
malloc_usable_size(const void *addr)
{
	return (addr == NULL ? 0 : xg_hdr_of(addr)->size);
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
	old = xg_hdr_of(addr)->size;
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
	if (addr != NULL)
		IOFreeContiguous(addr, round_page(size));
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
