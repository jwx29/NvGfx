/*
 * XeGfx: Intel stolen memory discovery, the legacy GMCH GTT entry points, LinuxKPI's ACPI notifier hooks and the
 * FreeBSD backlight(9) registry on XNU.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/sx.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/kobj.h>
#include <sys/queue.h>
#include <machine/md_var.h>
#include <vm/vm.h>
#include <dev/backlight/backlight.h>

#include <drm/intel/pciids.h>

#include "backlight_if.h"
#include "xg_dma.h"

static MALLOC_DEFINE(M_XG_DMA, "xg_dma", "XeGfx dma area");

static struct sx xg_dma_sx;	/* notifier list and backlight list */
static int xg_dma_up;

/* The sx needs xg_dma_init(); failing loudly beats sleeping on an uninitialised lock. */
#define	XG_DMA_ASSERT_UP(fn) do {					\
	if (!xg_dma_up)							\
		panic("xegfx: %s before xg_dma_init", fn);		\
} while (0)

/* Stolen memory (DSM). drm-kmod's i915_init copies these into intel_graphics_stolen_res. */

vm_paddr_t intel_graphics_stolen_base;
vm_paddr_t intel_graphics_stolen_size;

#define XG_PCI_ID		0x00
#define XG_PCI_CLASS		0x08
#define XG_GMCH_CTRL		0x50	/* SNB_GMCH_CTRL / GGC, 16 bits */
#define XG_BSM			0x5c	/* INTEL_BSM, gen6 to gen10 */
#define XG_BSM_DW0		0xc0	/* INTEL_GEN11_BSM_DW0 */
#define XG_BSM_DW1		0xc4
#define XG_BSM_MASK		0xfff00000u
#define XG_MB			(1024ull * 1024)

#define XG_ID(id, ...)		(id)
static const uint16_t xg_dma_gen11_ids[] = {
	INTEL_ICL_IDS(XG_ID), INTEL_EHL_IDS(XG_ID), INTEL_JSL_IDS(XG_ID), INTEL_TGL_IDS(XG_ID),
	INTEL_RKL_IDS(XG_ID), INTEL_ADLS_IDS(XG_ID), INTEL_ADLP_IDS(XG_ID), INTEL_ADLN_IDS(XG_ID),
	INTEL_RPLS_IDS(XG_ID), INTEL_RPLU_IDS(XG_ID), INTEL_RPLP_IDS(XG_ID),
};
/* Graphics IP 12.70+: i915 takes stolen memory from LMEMBAR and GGC itself and ignores the globals. */
static const uint16_t xg_dma_mtl_ids[] = {
	INTEL_MTL_IDS(XG_ID), INTEL_ARL_IDS(XG_ID),
};

static int
xg_dma_id_in(uint16_t id, const uint16_t *tab, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (tab[i] == id)
			return (1);
	return (0);
}

/* gen9_stolen_size() in Linux early-quirks; MTL's GGC uses the same encoding with fewer valid values. */
static uint64_t
xg_dma_gms_size(uint16_t ggc, int mtl)
{
	u_int gms = (ggc >> 8) & 0xff;

	if (gms >= 0xf0 && gms <= 0xfe)
		return ((uint64_t)(gms - 0xf0 + 1) * 4 * XG_MB);
	if (mtl && gms > 0x04)
		return (0);
	return (gms < 0xf0 ? (uint64_t)gms * 32 * XG_MB : 0);
}

static void
xg_dma_stolen_init(void)
{
	u_int id, cls, ggc, lo, hi;
	uint16_t dev;
	int gen11, mtl;

	intel_graphics_stolen_base = 0;
	intel_graphics_stolen_size = 0;
	/* The integrated GPU is always 0:2.0. Config reads are side-effect free, so this is safe unclaimed. */
	if (xg_dma_xnu_cfg_read32(0, 2, 0, XG_PCI_ID, &id) != 0 || (id & 0xffff) != 0x8086)
		return;
	if (xg_dma_xnu_cfg_read32(0, 2, 0, XG_PCI_CLASS, &cls) != 0 || (cls >> 24) != 0x03)
		return;
	dev = id >> 16;
	gen11 = xg_dma_id_in(dev, xg_dma_gen11_ids, nitems(xg_dma_gen11_ids));
	mtl = xg_dma_id_in(dev, xg_dma_mtl_ids, nitems(xg_dma_mtl_ids));
	if (!gen11 && !mtl) {
		printf("xegfx: iGPU 8086:%04x is older than gen11, stolen memory not probed\n", dev);
		return;
	}
	if (xg_dma_xnu_cfg_read32(0, 2, 0, XG_GMCH_CTRL & ~3u, &ggc) != 0 ||
	    xg_dma_xnu_cfg_read32(0, 2, 0, XG_BSM_DW0, &lo) != 0 ||
	    xg_dma_xnu_cfg_read32(0, 2, 0, XG_BSM_DW1, &hi) != 0)
		return;
	intel_graphics_stolen_base = ((vm_paddr_t)hi << 32) | (lo & XG_BSM_MASK);
	intel_graphics_stolen_size = xg_dma_gms_size(ggc & 0xffff, mtl);
	if (intel_graphics_stolen_base == 0)
		intel_graphics_stolen_size = 0;
	printf("xegfx: iGPU 8086:%04x stolen base 0x%jx size %ju MiB%s\n", dev,
	    (uintmax_t)intel_graphics_stolen_base, (uintmax_t)(intel_graphics_stolen_size / XG_MB),
	    mtl ? " (unused: i915 reads GGC/LMEMBAR on this IP)" : "");
}

/* Bus addresses. The shim hands out physical addresses as DMA addresses, which holds only without VT-d. */

int
xg_dma_iommu_active(void)
{
	return (xg_dma_xnu_iommu_state() != XG_DMA_IOMMU_NONE);
}

/*
 * Legacy GMCH GTT (FreeBSD agp_i810). i915 calls these only for GRAPHICS_VER < 6, which XeGfx does not support.
 * The flush is a real no-op: gen6+ has no chipset flush.
 */

/* Layout from dev/agp/agp_i810.h, which drags in vm_page.h and pmap.h. */
struct intel_gtt {
	unsigned int stolen_size;
	unsigned int gtt_total_entries;
	unsigned int gtt_mappable_entries;
	unsigned int needs_dmar : 1;
	unsigned int do_idle_maps : 1;
	vm_paddr_t scratch_page_dma;
	vm_page_t scratch_page;
	uint32_t *gtt;
	bus_addr_t gma_bus_addr;
};
struct intel_gtt *intel_gtt_get(void);
int intel_gtt_chipset_flush(void);
void intel_gtt_clear_range(u_int first_entry, u_int num_entries);
void intel_gtt_install_pte(u_int index, vm_paddr_t addr, u_int flags);
u_int32_t intel_gtt_read_pte(u_int entry);

static struct intel_gtt xg_dma_gtt;

struct intel_gtt *
intel_gtt_get(void)
{
	return (&xg_dma_gtt);
}

int
intel_gtt_chipset_flush(void)
{
	return (0);
}

void
intel_gtt_clear_range(u_int first_entry, u_int num_entries)
{
	xg_unimpl("intel_gtt_clear_range");
}

void
intel_gtt_install_pte(u_int index, vm_paddr_t addr, u_int flags)
{
	xg_unimpl("intel_gtt_install_pte");
}

u_int32_t
intel_gtt_read_pte(u_int entry)
{
	xg_unimpl("intel_gtt_read_pte");
}

/*
 * ACPI notifiers. FreeBSD hooks acpi_acad_event and acpi_video_event; XNU has no such events for kexts, so the
 * IOKit side feeds them through xg_dma_acpi_notify(). Delivery holds the list shared, like Linux's rwsem chain,
 * so a callback must not unregister itself.
 */

/*
 * LinuxKPI's headers cannot be mixed into a shim file yet (linux/list.h pulls machine/pmap.h), so these mirror
 * the leading members of struct notifier_block (linux/notifier.h) and all of struct acpi_bus_event (acpi_bus.h).
 */
struct notifier_block;
struct xg_dma_nb {
	int (*notifier_call)(struct notifier_block *, unsigned long, void *);
	struct notifier_block *next;
};
#define	XG_NB(p)	((struct xg_dma_nb *)(p))
struct xg_dma_acpi_bus_event {
	char device_class[20];
	uint32_t type;
	uint32_t data;
};
int register_acpi_notifier(struct notifier_block *nb);
int unregister_acpi_notifier(struct notifier_block *nb);
uint32_t acpi_target_system_state(void);

#define	ACPI_AC_CLASS			"ac_adapter"
#define	ACPI_VIDEO_CLASS		"video"
#define	XG_ACPI_NOTIFY_BUS_CHECK	0x00

static struct notifier_block *xg_dma_acpi_head;
static uint32_t xg_dma_acpi_state;	/* ACPI_STATE_S0; no sleep support yet */

int
register_acpi_notifier(struct notifier_block *nb)
{
	struct notifier_block *p;

	XG_DMA_ASSERT_UP(__func__);

	sx_xlock(&xg_dma_sx);
	for (p = xg_dma_acpi_head; p != NULL; p = XG_NB(p)->next)
		if (p == nb) {
			sx_xunlock(&xg_dma_sx);
			return (0);
		}
	XG_NB(nb)->next = xg_dma_acpi_head;
	xg_dma_acpi_head = nb;
	sx_xunlock(&xg_dma_sx);
	return (0);
}

int
unregister_acpi_notifier(struct notifier_block *nb)
{
	struct notifier_block **pp;

	XG_DMA_ASSERT_UP(__func__);

	sx_xlock(&xg_dma_sx);
	for (pp = &xg_dma_acpi_head; *pp != NULL; pp = &XG_NB(*pp)->next)
		if (*pp == nb) {
			*pp = XG_NB(nb)->next;
			XG_NB(nb)->next = NULL;
			break;
		}
	sx_xunlock(&xg_dma_sx);
	return (0);
}

void
xg_dma_acpi_notify(int kind, unsigned int type, unsigned int data)
{
	struct xg_dma_acpi_bus_event abe;
	struct notifier_block *p;

	XG_DMA_ASSERT_UP(__func__);

	memset(&abe, 0, sizeof(abe));
	if (kind == XG_DMA_ACPI_AC) {
		strlcpy(abe.device_class, ACPI_AC_CLASS, sizeof(abe.device_class));
		abe.type = XG_ACPI_NOTIFY_BUS_CHECK;	/* the type is lost on FreeBSD too; drm does not look */
		abe.data = data;
	} else {
		strlcpy(abe.device_class, ACPI_VIDEO_CLASS, sizeof(abe.device_class));
		abe.type = type;
	}
	sx_slock(&xg_dma_sx);
	for (p = xg_dma_acpi_head; p != NULL; p = XG_NB(p)->next)
		XG_NB(p)->notifier_call(p, 0, &abe);
	sx_sunlock(&xg_dma_sx);
}

uint32_t
acpi_target_system_state(void)
{
	return (atomic_load_acq_32(&xg_dma_acpi_state));
}

void
xg_dma_acpi_set_target_state(unsigned int state)
{
	atomic_store_rel_32(&xg_dma_acpi_state, state);
}

/*
 * backlight(9). FreeBSD makes /dev/backlight/backlightN for backlightctl; here the cdev is bookkeeping only and
 * the IOKit side reaches a backlight by unit through xg_dma_backlight_get/set.
 */

static int
xg_dma_kobj_error(void)
{
	return (ENXIO);
}

struct kobjop_desc backlight_update_status_desc = {
	0, { &backlight_update_status_desc, (kobjop_t)xg_dma_kobj_error }
};
struct kobjop_desc backlight_get_status_desc = {
	0, { &backlight_get_status_desc, (kobjop_t)xg_dma_kobj_error }
};
struct kobjop_desc backlight_get_info_desc = {
	0, { &backlight_get_info_desc, (kobjop_t)xg_dma_kobj_error }
};

struct xg_dma_bl {
	TAILQ_ENTRY(xg_dma_bl) link;
	struct cdev *cdev;
	device_t dev;
	int unit;
	uint32_t cached_brightness;
};
static TAILQ_HEAD(, xg_dma_bl) xg_dma_bls = TAILQ_HEAD_INITIALIZER(xg_dma_bls);

static int
xg_dma_bl_unit(void)
{
	struct xg_dma_bl *b;
	int unit;

	/* Lowest free unit, like alloc_unr(). Caller holds xg_dma_sx exclusive. */
	for (unit = 0;; unit++) {
		TAILQ_FOREACH(b, &xg_dma_bls, link)
			if (b->unit == unit)
				break;
		if (b == NULL)
			return (unit);
	}
}

static int
xg_dma_bl_has_ops(device_t dev)
{
	/* A device_t without kobj ops would fault in KOBJOPLOOKUP. */
	return (dev != NULL && ((kobj_t)dev)->ops != NULL);
}

struct cdev *
backlight_register(const char *name, device_t dev)
{
	struct backlight_props props;
	struct xg_dma_bl *b;
	struct cdev *cd;

	XG_DMA_ASSERT_UP(__func__);

	b = malloc(sizeof(*b), M_XG_DMA, M_WAITOK | M_ZERO);
	cd = malloc(sizeof(*cd), M_XG_DMA, M_WAITOK | M_ZERO);
	b->cdev = cd;
	b->dev = dev;
	cd->si_drv1 = b;

	sx_xlock(&xg_dma_sx);
	b->unit = xg_dma_bl_unit();
	snprintf(cd->si_name, sizeof(cd->si_name), "backlight/backlight%d", b->unit);
	TAILQ_INSERT_TAIL(&xg_dma_bls, b, link);
	sx_xunlock(&xg_dma_sx);
	printf("xegfx: backlight%d registered (%s%d)\n", b->unit, name, b->unit);

	memset(&props, 0, sizeof(props));
	if (xg_dma_bl_has_ops(dev) && BACKLIGHT_GET_STATUS(dev, &props) == 0)
		b->cached_brightness = props.brightness;
	return (cd);
}

int
backlight_destroy(struct cdev *dev)
{
	struct xg_dma_bl *b;

	XG_DMA_ASSERT_UP(__func__);

	if (dev == NULL)
		return (0);
	b = dev->si_drv1;
	sx_xlock(&xg_dma_sx);
	TAILQ_REMOVE(&xg_dma_bls, b, link);
	sx_xunlock(&xg_dma_sx);
	free(dev, M_XG_DMA);
	free(b, M_XG_DMA);
	return (0);
}

/* Same path as the BACKLIGHTGETSTATUS/BACKLIGHTUPDATESTATUS ioctls. Thread context only: drivers may sleep. */

static struct xg_dma_bl *
xg_dma_bl_find(int unit)
{
	struct xg_dma_bl *b;

	TAILQ_FOREACH(b, &xg_dma_bls, link)
		if (b->unit == unit)
			return (b);
	return (NULL);
}

int
xg_dma_backlight_get(int unit, unsigned int *brightness)
{
	struct backlight_props props;
	struct xg_dma_bl *b;
	int error;

	XG_DMA_ASSERT_UP(__func__);

	sx_slock(&xg_dma_sx);
	b = xg_dma_bl_find(unit);
	if (b == NULL || !xg_dma_bl_has_ops(b->dev)) {
		sx_sunlock(&xg_dma_sx);
		return (ENODEV);
	}
	memset(&props, 0, sizeof(props));
	error = BACKLIGHT_GET_STATUS(b->dev, &props);
	if (error == 0) {
		b->cached_brightness = props.brightness;
		*brightness = props.brightness;
	}
	sx_sunlock(&xg_dma_sx);
	return (error);
}

int
xg_dma_backlight_set(int unit, unsigned int brightness)
{
	struct backlight_props props;
	struct xg_dma_bl *b;
	int error;

	XG_DMA_ASSERT_UP(__func__);

	/* Exclusive: the cached value is read and written here. */
	sx_xlock(&xg_dma_sx);
	b = xg_dma_bl_find(unit);
	if (b == NULL || !xg_dma_bl_has_ops(b->dev)) {
		sx_xunlock(&xg_dma_sx);
		return (ENODEV);
	}
	if (brightness == b->cached_brightness) {
		sx_xunlock(&xg_dma_sx);
		return (0);
	}
	memset(&props, 0, sizeof(props));
	props.brightness = brightness;
	error = BACKLIGHT_UPDATE_STATUS(b->dev, &props);
	if (error == 0)
		b->cached_brightness = props.brightness;
	sx_xunlock(&xg_dma_sx);
	return (error);
}

/* Called once from the kext start, after xg_kern_init and before any drm or i915 module init. */
void
xg_dma_init(void)
{
	if (xg_dma_up)
		return;
	sx_init(&xg_dma_sx, "xg_dma");
	xg_dma_up = 1;
	xg_dma_stolen_init();
	switch (xg_dma_xnu_iommu_state()) {
	case XG_DMA_IOMMU_NONE:
		printf("xegfx: no IOMMU, DMA addresses are physical\n");
		break;
	case XG_DMA_IOMMU_ACTIVE:
		printf("xegfx: VT-d remapping is active, 1:1 DMA addresses will not work\n");
		break;
	default:
		printf("xegfx: IOMMU state not known yet\n");
		break;
	}
}
