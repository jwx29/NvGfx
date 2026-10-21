/*
 * XeGfx: FreeBSD newbus (kobj, devclass, device_t), resources, bus_dma tags and the PCI bus on top of XNU's
 * IOPCIDevice, enough for LinuxKPI's linux_pci.c to probe a Linux pci_driver on a device that IOKit hands over.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/pciio.h>
#include <sys/sysctl.h>
#include <sys/interrupt.h>
#include <sys/devctl.h>
#include <sys/stdarg.h>
#include <machine/atomic.h>
#include <machine/cpufunc.h>
#include <machine/cpu.h>
#include <machine/bus.h>
#include <machine/resource.h>
#include <vm/vm.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pci_private.h>
#include "xg_pci_xnu.h"

/* vm/pmap.h drags in per-CPU inlines the shim build rejects; this is the one call needed (vm area) */
vm_paddr_t pmap_kextract(vm_offset_t va);

static MALLOC_DEFINE(M_XGBUS, "xgbus", "newbus emulation");

_Static_assert(XG_PCI_FILTER_SCHEDULE == FILTER_SCHEDULE_THREAD, "filter bits");

/* x86 MSI numbering as LinuxKPI sees it (x86/intr_machdep.h); our table covers INTx 1..255 and MSI 256..511 */
u_int first_msi_irq = 256;
u_int num_msi_irqs = 256;
#define XG_PCI_NIRQ	512

struct devlist pci_devq = STAILQ_HEAD_INITIALIZER(pci_devq);

static struct mtx xg_pci_kobj_mtx;	/* kobj class compile and refs */
static struct mtx xg_pci_topo_mtx;	/* the device tree, bus_topo_lock() */
static struct mtx xg_pci_irq_mtx;	/* the irq table and installed handlers */
static volatile u_int xg_pci_inited;

static void xg_pci_once(void);

/*
 * kobj (sys/kern/subr_kobj.c)
 */

static const struct kobj_method xg_pci_null_method = { 0, 0 };
static int xg_pci_kobj_next_id = 1;

int
kobj_error_method(void)
{
	return (ENXIO);
}

static void
xg_pci_kobj_total_size(kobj_class_t cls)
{
	kobj_class_t *bases = cls->baseclasses;
	size_t size = roundup2(cls->size, _Alignof(__max_align_t));

	if (bases != NULL) {
		for (int i = 0; bases[i] != NULL; i++) {
			if (!bases[i]->total_size_inited)
				xg_pci_kobj_total_size(bases[i]);
			size += bases[i]->total_size;
		}
	}
	cls->total_size = size;
	cls->total_size_inited = true;
}

void
kobj_class_compile(kobj_class_t cls)
{
	kobj_ops_t ops;
	kobj_method_t *m;

	xg_pci_once();
	ops = malloc(sizeof(*ops), M_XGBUS, M_WAITOK);
	mtx_lock(&xg_pci_kobj_mtx);
	if (cls->ops != NULL) {
		/* lost the race to another compile */
		mtx_unlock(&xg_pci_kobj_mtx);
		free(ops, M_XGBUS);
		return;
	}
	for (m = cls->methods; m->desc != NULL; m++)
		if (m->desc->id == 0)
			m->desc->id = xg_pci_kobj_next_id++;
	for (int i = 0; i < KOBJ_CACHE_SIZE; i++)
		ops->cache[i] = &xg_pci_null_method;
	ops->cls = cls;
	cls->ops = ops;
	if (!cls->total_size_inited)
		xg_pci_kobj_total_size(cls);
	mtx_unlock(&xg_pci_kobj_mtx);
}

void
kobj_class_free(kobj_class_t cls)
{
	void *ops = NULL;

	mtx_lock(&xg_pci_kobj_mtx);
	if (cls->refs == 0) {
		ops = cls->ops;
		cls->ops = NULL;
	}
	mtx_unlock(&xg_pci_kobj_mtx);
	if (ops != NULL)
		free(ops, M_XGBUS);
}

static kobj_method_t *
xg_pci_kobj_find(kobj_class_t cls, kobjop_desc_t desc)
{
	kobj_method_t *ce;
	kobj_class_t *basep;

	for (ce = cls->methods; ce != NULL && ce->desc != NULL; ce++)
		if (ce->desc == desc)
			return (ce);
	if ((basep = cls->baseclasses) != NULL)
		for (; *basep != NULL; basep++)
			if ((ce = xg_pci_kobj_find(*basep, desc)) != NULL)
				return (ce);
	return (NULL);
}

kobj_method_t *
kobj_lookup_method(kobj_class_t cls, kobj_method_t **cep, kobjop_desc_t desc)
{
	kobj_method_t *ce;

	ce = xg_pci_kobj_find(cls, desc);
	if (ce == NULL)
		ce = &desc->deflt;
	if (cep != NULL)
		*cep = ce;
	return (ce);
}

void
kobj_init(kobj_t obj, kobj_class_t cls)
{
	mtx_lock(&xg_pci_kobj_mtx);
	while (cls->ops == NULL) {
		/* compile mallocs, so not under the lock */
		mtx_unlock(&xg_pci_kobj_mtx);
		kobj_class_compile(cls);
		mtx_lock(&xg_pci_kobj_mtx);
	}
	obj->ops = cls->ops;
	cls->refs++;
	mtx_unlock(&xg_pci_kobj_mtx);
}

void
kobj_delete(kobj_t obj, struct malloc_type *mtype)
{
	kobj_class_t cls = obj->ops->cls;
	u_int refs;

	mtx_lock(&xg_pci_kobj_mtx);
	refs = --cls->refs;
	mtx_unlock(&xg_pci_kobj_mtx);
	if (refs == 0)
		kobj_class_free(cls);
	obj->ops = NULL;
	if (mtype != NULL)
		free(obj, mtype);
}

size_t
kobj_total_data_size(kobj_class_t cls)
{
	return (cls->total_size);
}

/*
 * Method descriptors (the generated *_if.c files), defaults as in the .m files
 */

static int
xg_pci_null_zero(void)
{
	return (0);
}

static struct resource *
xg_pci_null_alloc_resource(device_t dev, device_t child, int type, int rid, rman_res_t start, rman_res_t end,
    rman_res_t count, u_int flags)
{
	return (NULL);
}

static device_t
xg_pci_null_add_child(device_t bus, u_int order, const char *name, int unit)
{
	panic("%s: bus_add_child is not implemented", device_get_nameunit(bus));
}

#define XG_DESC(name, def) \
	struct kobjop_desc name##_desc = { 0, { &name##_desc, (kobjop_t)(def) } }

XG_DESC(device_probe, kobj_error_method);
XG_DESC(device_attach, kobj_error_method);
XG_DESC(device_detach, kobj_error_method);
XG_DESC(device_shutdown, xg_pci_null_zero);
XG_DESC(device_suspend, xg_pci_null_zero);
XG_DESC(device_resume, xg_pci_null_zero);
XG_DESC(bus_read_ivar, kobj_error_method);
XG_DESC(bus_add_child, xg_pci_null_add_child);
XG_DESC(bus_driver_added, bus_generic_driver_added);
XG_DESC(bus_alloc_resource, xg_pci_null_alloc_resource);
XG_DESC(bus_activate_resource, kobj_error_method);
XG_DESC(bus_deactivate_resource, kobj_error_method);
XG_DESC(bus_release_resource, kobj_error_method);
XG_DESC(bus_translate_resource, bus_generic_translate_resource);
XG_DESC(bus_setup_intr, kobj_error_method);
XG_DESC(bus_teardown_intr, kobj_error_method);
XG_DESC(bus_bind_intr, bus_generic_bind_intr);
XG_DESC(bus_child_present, bus_generic_child_present);
XG_DESC(bus_get_dma_tag, bus_generic_get_dma_tag);
XG_DESC(bus_get_domain, bus_generic_get_domain);
XG_DESC(pci_read_config, kobj_error_method);
XG_DESC(pci_write_config, kobj_error_method);
XG_DESC(pci_enable_busmaster, kobj_error_method);
XG_DESC(pci_disable_busmaster, kobj_error_method);
XG_DESC(pci_enable_io, kobj_error_method);
XG_DESC(pci_disable_io, kobj_error_method);
XG_DESC(pci_get_powerstate, kobj_error_method);
XG_DESC(pci_set_powerstate, kobj_error_method);
XG_DESC(pci_alloc_msi, kobj_error_method);
XG_DESC(pci_alloc_msix, kobj_error_method);
XG_DESC(pci_release_msi, kobj_error_method);
XG_DESC(pci_msi_count, xg_pci_null_zero);
XG_DESC(pci_msix_count, xg_pci_null_zero);
XG_DESC(pci_get_id, kobj_error_method);
/* SR-IOV: only the LinuxKPI driver class names these, nothing calls them on XNU */
XG_DESC(pci_iov_init, kobj_error_method);
XG_DESC(pci_iov_uninit, kobj_error_method);
XG_DESC(pci_iov_add_vf, kobj_error_method);

/*
 * Newbus core (sys/kern/subr_bus.c)
 */

struct driverlink {
	kobj_class_t	driver;
	TAILQ_ENTRY(driverlink) link;
	int		pass;
};

struct devclass {
	TAILQ_ENTRY(devclass) link;
	devclass_t	parent;
	TAILQ_HEAD(, driverlink) drivers;
	char		*name;
	device_t	*devices;
	int		maxunit;
	int		flags;
#define DC_HAS_CHILDREN	1
};

struct _device {
	KOBJ_FIELDS;			/* first: a device is a kobj */
	TAILQ_ENTRY(_device) link;	/* in parent's children */
	TAILQ_ENTRY(_device) devlink;	/* in xg_pci_devices */
	device_t	parent;
	TAILQ_HEAD(xg_pci_devlist, _device) children;
	driver_t	*driver;
	devclass_t	devclass;
	int		unit;
	char		*nameunit;
	char		*desc;
	u_int		busy;
	device_state_t	state;
	uint32_t	devflags;
	u_int		flags;
	u_int		order;
	void		*ivars;
	void		*softc;
	struct sysctl_ctx_list sysctl_ctx;
};

static TAILQ_HEAD(, devclass) xg_pci_devclasses = TAILQ_HEAD_INITIALIZER(xg_pci_devclasses);
static TAILQ_HEAD(, _device) xg_pci_devices = TAILQ_HEAD_INITIALIZER(xg_pci_devices);

static kobj_method_t xg_pci_null_methods[] = { KOBJMETHOD_END };
static DEFINE_CLASS_0(null, xg_pci_null_class, xg_pci_null_methods, 0);

void
bus_topo_lock(void)
{
	xg_pci_once();
	mtx_lock(&xg_pci_topo_mtx);
}

void
bus_topo_unlock(void)
{
	mtx_unlock(&xg_pci_topo_mtx);
}

struct mtx *
bus_topo_mtx(void)
{
	xg_pci_once();
	return (&xg_pci_topo_mtx);
}

void
bus_topo_assert(void)
{
}

static devclass_t
xg_pci_devclass(const char *classname, const char *parentname, int create)
{
	devclass_t dc;

	if (classname == NULL)
		return (NULL);
	xg_pci_once();
	TAILQ_FOREACH(dc, &xg_pci_devclasses, link)
		if (strcmp(dc->name, classname) == 0)
			break;
	if (create && dc == NULL) {
		dc = malloc(sizeof(*dc) + strlen(classname) + 1, M_XGBUS, M_WAITOK | M_ZERO);
		dc->name = (char *)(dc + 1);
		strcpy(dc->name, classname);
		TAILQ_INIT(&dc->drivers);
		TAILQ_INSERT_TAIL(&xg_pci_devclasses, dc, link);
	}
	/* a driver with a base class also searches the base's devclass, but never itself */
	if (parentname != NULL && dc != NULL && dc->parent == NULL && strcmp(classname, parentname) != 0) {
		dc->parent = xg_pci_devclass(parentname, NULL, TRUE);
		dc->parent->flags |= DC_HAS_CHILDREN;
	}
	return (dc);
}

devclass_t
devclass_create(const char *classname)
{
	return (xg_pci_devclass(classname, NULL, TRUE));
}

devclass_t
devclass_find(const char *classname)
{
	return (xg_pci_devclass(classname, NULL, FALSE));
}

const char *
devclass_get_name(devclass_t dc)
{
	return (dc->name);
}

device_t
devclass_get_device(devclass_t dc, int unit)
{
	if (dc == NULL || unit < 0 || unit >= dc->maxunit)
		return (NULL);
	return (dc->devices[unit]);
}

void *
devclass_get_softc(devclass_t dc, int unit)
{
	device_t dev = devclass_get_device(dc, unit);

	return (dev != NULL ? dev->softc : NULL);
}

int
devclass_get_maxunit(devclass_t dc)
{
	return (dc != NULL ? dc->maxunit : -1);
}

static void
xg_pci_driver_added(devclass_t dc, driver_t *driver)
{
	devclass_t parent;

	for (int i = 0; i < dc->maxunit; i++)
		if (dc->devices[i] != NULL && device_is_attached(dc->devices[i]))
			BUS_DRIVER_ADDED(dc->devices[i], driver);
	if (!(dc->flags & DC_HAS_CHILDREN))
		return;
	parent = dc;
	TAILQ_FOREACH(dc, &xg_pci_devclasses, link)
		if (dc->parent == parent)
			xg_pci_driver_added(dc, driver);
}

int
devclass_add_driver(devclass_t dc, driver_t *driver, int pass, devclass_t *dcp)
{
	struct driverlink *dl;
	devclass_t child_dc;

	if (pass <= BUS_PASS_ROOT)
		return (EINVAL);
	dl = malloc(sizeof(*dl), M_XGBUS, M_WAITOK | M_ZERO);
	kobj_class_compile((kobj_class_t)driver);
	child_dc = xg_pci_devclass(driver->name, driver->baseclasses != NULL ? driver->baseclasses[0]->name : NULL,
	    TRUE);
	if (dcp != NULL)
		*dcp = child_dc;
	dl->driver = driver;
	dl->pass = pass;
	TAILQ_INSERT_TAIL(&dc->drivers, dl, link);
	driver->refs++;
	/* there is one pass on XNU (BUS_PASS_DEFAULT), so a new driver probes right away */
	xg_pci_driver_added(dc, driver);
	return (0);
}

static int
xg_pci_driver_deleted(devclass_t busclass, devclass_t dc, driver_t *driver)
{
	devclass_t parent;
	device_t dev;
	int error;

	for (int i = 0; i < dc->maxunit; i++) {
		dev = dc->devices[i];
		if (dev != NULL && dev->driver == driver && dev->parent != NULL && dev->parent->devclass == busclass)
			if ((error = device_detach(dev)) != 0)
				return (error);
	}
	if (!(busclass->flags & DC_HAS_CHILDREN))
		return (0);
	parent = busclass;
	TAILQ_FOREACH(busclass, &xg_pci_devclasses, link)
		if (busclass->parent == parent)
			if ((error = xg_pci_driver_deleted(busclass, dc, driver)) != 0)
				return (error);
	return (0);
}

int
devclass_delete_driver(devclass_t busclass, driver_t *driver)
{
	devclass_t dc = devclass_find(driver->name);
	struct driverlink *dl;
	int error;

	if (dc == NULL)
		return (0);
	TAILQ_FOREACH(dl, &busclass->drivers, link)
		if (dl->driver == driver)
			break;
	if (dl == NULL)
		return (ENOENT);
	if ((error = xg_pci_driver_deleted(busclass, dc, driver)) != 0)
		return (error);
	TAILQ_REMOVE(&busclass->drivers, dl, link);
	free(dl, M_XGBUS);
	if (--driver->refs == 0)
		kobj_class_free((kobj_class_t)driver);
	return (0);
}

static int
xg_pci_alloc_unit(devclass_t dc, int *unitp)
{
	int unit = *unitp;

	if ((unit < 0 && unit != DEVICE_UNIT_ANY) || unit == INT_MAX)
		return (EINVAL);
	if (unit != DEVICE_UNIT_ANY) {
		if (unit < dc->maxunit && dc->devices[unit] != NULL)
			return (EEXIST);
	} else {
		for (unit = 0; unit < dc->maxunit && dc->devices[unit] != NULL; unit++)
			;
	}
	if (unit >= dc->maxunit) {
		device_t *n = malloc((unit + 1) * sizeof(*n), M_XGBUS, M_WAITOK | M_ZERO);

		if (dc->devices != NULL) {
			memcpy(n, dc->devices, dc->maxunit * sizeof(*n));
			free(dc->devices, M_XGBUS);
		}
		dc->devices = n;
		dc->maxunit = unit + 1;
	}
	*unitp = unit;
	return (0);
}

static int
xg_pci_devclass_add_device(devclass_t dc, device_t dev)
{
	int buflen, error;

	buflen = snprintf(NULL, 0, "%s%d$", dc->name, INT_MAX);
	dev->nameunit = malloc(buflen, M_XGBUS, M_WAITOK | M_ZERO);
	if ((error = xg_pci_alloc_unit(dc, &dev->unit)) != 0) {
		free(dev->nameunit, M_XGBUS);
		dev->nameunit = NULL;
		return (error);
	}
	dc->devices[dev->unit] = dev;
	dev->devclass = dc;
	snprintf(dev->nameunit, buflen, "%s%d", dc->name, dev->unit);
	return (0);
}

static void
xg_pci_devclass_delete_device(devclass_t dc, device_t dev)
{
	if (dc == NULL || dev == NULL)
		return;
	if (dev->devclass != dc || dc->devices[dev->unit] != dev)
		panic("devclass_delete_device: inconsistent device class");
	dc->devices[dev->unit] = NULL;
	if (dev->flags & DF_WILDCARD)
		dev->unit = DEVICE_UNIT_ANY;
	dev->devclass = NULL;
	free(dev->nameunit, M_XGBUS);
	dev->nameunit = NULL;
}

static device_t
xg_pci_make_device(device_t parent, const char *name, int unit)
{
	devclass_t dc = NULL;
	device_t dev;

	if (name != NULL && (dc = xg_pci_devclass(name, NULL, TRUE)) == NULL)
		return (NULL);
	dev = malloc(sizeof(*dev), M_XGBUS, M_WAITOK | M_ZERO);
	dev->parent = parent;
	TAILQ_INIT(&dev->children);
	TAILQ_INIT(&dev->sysctl_ctx);
	kobj_init((kobj_t)dev, &xg_pci_null_class);
	dev->unit = unit;
	dev->flags = DF_ENABLED;
	if (unit == DEVICE_UNIT_ANY)
		dev->flags |= DF_WILDCARD;
	if (name != NULL) {
		dev->flags |= DF_FIXEDCLASS;
		if (xg_pci_devclass_add_device(dc, dev) != 0) {
			kobj_delete((kobj_t)dev, M_XGBUS);
			return (NULL);
		}
	}
	dev->state = DS_NOTPRESENT;
	TAILQ_INSERT_TAIL(&xg_pci_devices, dev, devlink);
	return (dev);
}

device_t
device_add_child_ordered(device_t dev, u_int order, const char *name, int unit)
{
	device_t child, place;

	child = xg_pci_make_device(dev, name, unit);
	if (child == NULL)
		return (NULL);
	child->order = order;
	TAILQ_FOREACH(place, &dev->children, link)
		if (place->order > order)
			break;
	if (place != NULL)
		TAILQ_INSERT_BEFORE(place, child, link);
	else
		TAILQ_INSERT_TAIL(&dev->children, child, link);
	return (child);
}

device_t
device_add_child(device_t dev, const char *name, int unit)
{
	return (device_add_child_ordered(dev, 0, name, unit));
}

int
device_delete_child(device_t dev, device_t child)
{
	device_t grandchild;
	int error;

	if ((error = device_detach(child)) != 0)
		return (error);
	while ((grandchild = TAILQ_FIRST(&child->children)) != NULL)
		if ((error = device_delete_child(child, grandchild)) != 0)
			return (error);
	if (child->devclass != NULL)
		xg_pci_devclass_delete_device(child->devclass, child);
	/* drop what a detached driver left, as device_set_driver(NULL) would */
	if (child->softc != NULL && !(child->flags & DF_EXTERNALSOFTC))
		free(child->softc, M_XGBUS);
	if (child->desc != NULL && (child->flags & DF_DESCMALLOCED))
		free(child->desc, M_XGBUS);
	TAILQ_REMOVE(&dev->children, child, link);
	TAILQ_REMOVE(&xg_pci_devices, child, devlink);
	kobj_delete((kobj_t)child, M_XGBUS);
	return (0);
}

int
device_delete_children(device_t dev)
{
	device_t child;
	int error = 0;

	while ((child = TAILQ_FIRST(&dev->children)) != NULL)
		if ((error = device_delete_child(dev, child)) != 0)
			break;
	return (error);
}

device_t
device_get_parent(device_t dev)
{
	return (dev->parent);
}

driver_t *
device_get_driver(device_t dev)
{
	return (dev->driver);
}

devclass_t
device_get_devclass(device_t dev)
{
	return (dev->devclass);
}

const char *
device_get_name(device_t dev)
{
	if (dev != NULL && dev->devclass != NULL)
		return (dev->devclass->name);
	return (NULL);
}

const char *
device_get_nameunit(device_t dev)
{
	return (dev->nameunit);
}

int
device_get_unit(device_t dev)
{
	return (dev->unit);
}

const char *
device_get_desc(device_t dev)
{
	return (dev->desc);
}

device_state_t
device_get_state(device_t dev)
{
	return (dev->state);
}

uint32_t
device_get_flags(device_t dev)
{
	return (dev->devflags);
}

void *
device_get_ivars(device_t dev)
{
	KASSERT(dev != NULL, ("device_get_ivars(NULL, ...)"));
	return (dev->ivars);
}

void
device_set_ivars(device_t dev, void *ivars)
{
	KASSERT(dev != NULL, ("device_set_ivars(NULL, ...)"));
	dev->ivars = ivars;
}

void *
device_get_softc(device_t dev)
{
	return (dev->softc);
}

void
device_set_softc(device_t dev, void *softc)
{
	if (dev->softc != NULL && !(dev->flags & DF_EXTERNALSOFTC))
		free(dev->softc, M_XGBUS);
	dev->softc = softc;
	if (dev->softc != NULL)
		dev->flags |= DF_EXTERNALSOFTC;
	else
		dev->flags &= ~DF_EXTERNALSOFTC;
}

struct sysctl_ctx_list *
device_get_sysctl_ctx(device_t dev)
{
	return (&dev->sysctl_ctx);
}

int
device_is_alive(device_t dev)
{
	return (dev->state >= DS_ALIVE);
}

int
device_is_attached(device_t dev)
{
	return (dev->state >= DS_ATTACHED);
}

int
device_is_enabled(device_t dev)
{
	return ((dev->flags & DF_ENABLED) != 0);
}

bool
device_has_children(device_t dev)
{
	return (!TAILQ_EMPTY(&dev->children));
}

int
device_get_children(device_t dev, device_t **devlistp, int *devcountp)
{
	device_t child, *list;
	int count = 0;

	TAILQ_FOREACH(child, &dev->children, link)
		count++;
	list = malloc((count ? count : 1) * sizeof(device_t), M_TEMP, M_WAITOK | M_ZERO);
	count = 0;
	TAILQ_FOREACH(child, &dev->children, link)
		list[count++] = child;
	*devlistp = list;
	*devcountp = count;
	return (0);
}

static void
xg_pci_set_desc(device_t dev, const char *desc, bool allocated)
{
	if (dev->desc != NULL && (dev->flags & DF_DESCMALLOCED)) {
		free(dev->desc, M_XGBUS);
		dev->flags &= ~DF_DESCMALLOCED;
	}
	dev->desc = NULL;
	if (allocated && desc != NULL)
		dev->flags |= DF_DESCMALLOCED;
	dev->desc = __DECONST(char *, desc);
}

void
device_set_desc(device_t dev, const char *desc)
{
	xg_pci_set_desc(dev, desc, false);
}

void
device_set_desc_copy(device_t dev, const char *desc)
{
	xg_pci_set_desc(dev, desc != NULL ? strdup(desc, M_XGBUS) : NULL, true);
}

int
device_print_prettyname(device_t dev)
{
	const char *name = device_get_name(dev);

	if (name == NULL)
		return (printf("unknown: "));
	return (printf("%s%d: ", name, device_get_unit(dev)));
}

int
device_printf(device_t dev, const char *fmt, ...)
{
	char buf[512];
	const char *name = device_get_name(dev);
	va_list ap;
	int n;

	/* one printf per line so concurrent messages do not interleave */
	if (name == NULL)
		n = snprintf(buf, sizeof(buf), "unknown: ");
	else
		n = snprintf(buf, sizeof(buf), "%s%d: ", name, device_get_unit(dev));
	if (n < 0 || n >= (int)sizeof(buf))
		n = 0;
	va_start(ap, fmt);
	vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	return (printf("%s", buf));
}

/* FreeBSD queues these for devd; nothing listens on XNU, which is what devctl does with no reader */
void
devctl_notify(const char *system, const char *subsystem, const char *type, const char *data)
{
}

int
device_set_devclass(device_t dev, const char *classname)
{
	devclass_t dc;

	if (classname == NULL) {
		if (dev->devclass != NULL)
			xg_pci_devclass_delete_device(dev->devclass, dev);
		return (0);
	}
	if (dev->devclass != NULL)
		return (EINVAL);
	if ((dc = xg_pci_devclass(classname, NULL, TRUE)) == NULL)
		return (ENOMEM);
	return (xg_pci_devclass_add_device(dc, dev));
}

int
device_set_driver(device_t dev, driver_t *driver)
{
	size_t size;

	if (dev->state >= DS_ATTACHED)
		return (EBUSY);
	if (dev->driver == driver)
		return (0);
	if (dev->softc != NULL && !(dev->flags & DF_EXTERNALSOFTC)) {
		free(dev->softc, M_XGBUS);
		dev->softc = NULL;
	}
	device_set_desc(dev, NULL);
	kobj_delete((kobj_t)dev, NULL);
	dev->driver = driver;
	if (driver != NULL) {
		kobj_init((kobj_t)dev, (kobj_class_t)driver);
		size = kobj_total_data_size(driver);
		if (!(dev->flags & DF_EXTERNALSOFTC) && size > 0)
			dev->softc = malloc(size, M_XGBUS, M_WAITOK | M_ZERO);
	} else
		kobj_init((kobj_t)dev, &xg_pci_null_class);
	return (0);
}

static struct driverlink *
xg_pci_first_driver(devclass_t dc, device_t dev)
{
	struct driverlink *dl;

	if (dev->devclass == NULL)
		return (TAILQ_FIRST(&dc->drivers));
	TAILQ_FOREACH(dl, &dc->drivers, link)
		if (strcmp(dl->driver->name, dev->devclass->name) == 0)
			return (dl);
	return (NULL);
}

static struct driverlink *
xg_pci_next_driver(devclass_t dc, device_t dev, struct driverlink *last)
{
	struct driverlink *dl;

	if (dev->devclass == NULL)
		return (TAILQ_NEXT(last, link));
	for (dl = TAILQ_NEXT(last, link); dl != NULL; dl = TAILQ_NEXT(dl, link))
		if (strcmp(dev->devclass->name, dl->driver->name) == 0)
			return (dl);
	return (NULL);
}

int
device_probe_child(device_t dev, device_t child)
{
	struct driverlink *best = NULL, *dl;
	int hasclass = (child->devclass != NULL);
	int result, pri = 0;
	devclass_t dc;

	if ((dc = dev->devclass) == NULL)
		panic("device_probe_child: parent device has no devclass");
	if (child->state == DS_ALIVE)
		return (0);
	for (; dc != NULL; dc = dc->parent) {
		for (dl = xg_pci_first_driver(dc, child); dl != NULL; dl = xg_pci_next_driver(dc, child, dl)) {
			result = device_set_driver(child, dl->driver);
			if (result == ENOMEM)
				return (result);
			if (result != 0)
				continue;
			if (!hasclass && device_set_devclass(child, dl->driver->name) != 0) {
				(void)device_set_driver(child, NULL);
				continue;
			}
			result = DEVICE_PROBE(child);
			if (result == 0) {
				best = dl;
				pri = 0;
				goto exact_match;
			}
			child->devflags = 0;
			if (!hasclass)
				(void)device_set_devclass(child, NULL);
			if (result <= BUS_PROBE_NOWILDCARD && !(child->flags & DF_FIXEDCLASS))
				result = ENXIO;
			if (result > 0) {
				(void)device_set_driver(child, NULL);
				continue;
			}
			if (best == NULL || result > pri) {
				best = dl;
				pri = result;
			}
		}
	}
	if (best == NULL)
		return (ENXIO);
	if ((result = device_set_driver(child, best->driver)) != 0)
		return (result);
	if (child->devclass == NULL && (result = device_set_devclass(child, best->driver->name)) != 0) {
		(void)device_set_driver(child, NULL);
		return (result);
	}
	/* probe the winner again so its description is the one that stays */
	result = DEVICE_PROBE(child);
	if (result > 0) {
		if (!hasclass)
			(void)device_set_devclass(child, NULL);
		(void)device_set_driver(child, NULL);
		return (result);
	}
exact_match:
	child->state = DS_ALIVE;
	return (0);
}

int
device_probe(device_t dev)
{
	if (dev->state >= DS_ALIVE)
		return (-1);
	if (!(dev->flags & DF_ENABLED))
		return (-1);
	return (device_probe_child(dev->parent, dev));
}

int
device_attach(device_t dev)
{
	int error;

	dev->state = DS_ATTACHING;
	if ((error = DEVICE_ATTACH(dev)) != 0) {
		printf("device_attach: %s%d attach returned %d\n", dev->driver->name, dev->unit, error);
		if (!(dev->flags & DF_FIXEDCLASS))
			xg_pci_devclass_delete_device(dev->devclass, dev);
		(void)device_set_driver(dev, NULL);
		dev->state = DS_NOTPRESENT;
		return (error);
	}
	dev->flags |= DF_ATTACHED_ONCE;
	dev->state = DS_ATTACHED;
	return (0);
}

int
device_probe_and_attach(device_t dev)
{
	int error;

	error = device_probe(dev);
	if (error == -1)
		return (0);
	if (error != 0)
		return (error);
	return (device_attach(dev));
}

int
device_detach(device_t dev)
{
	int error;

	if (dev->busy > 0)
		return (EBUSY);
	if (dev->state == DS_ATTACHING) {
		device_printf(dev, "device in attaching state! Deferring detach.\n");
		return (EBUSY);
	}
	if (dev->state != DS_ATTACHED)
		return (0);
	if ((error = DEVICE_DETACH(dev)) != 0)
		return (error);
	if (!(dev->flags & DF_FIXEDCLASS))
		xg_pci_devclass_delete_device(dev->devclass, dev);
	dev->state = DS_NOTPRESENT;
	(void)device_set_driver(dev, NULL);
	return (0);
}

int
device_shutdown(device_t dev)
{
	if (dev->state < DS_ATTACHED)
		return (0);
	return (DEVICE_SHUTDOWN(dev));
}

void
device_busy(device_t dev)
{
	dev->busy++;
}

void
device_unbusy(device_t dev)
{
	if (dev->busy > 0)
		dev->busy--;
}

void
bus_attach_children(device_t dev)
{
	device_t child;

	TAILQ_FOREACH(child, &dev->children, link)
		device_probe_and_attach(child);
}

int
bus_detach_children(device_t dev)
{
	device_t child;
	int error;

	/* reverse order, as FreeBSD does, so later children go first */
	TAILQ_FOREACH_REVERSE(child, &dev->children, xg_pci_devlist, link)
		if ((error = device_detach(child)) != 0)
			return (error);
	return (0);
}

/*
 * Generic bus methods
 */

device_t
bus_generic_add_child(device_t dev, u_int order, const char *name, int unit)
{
	return (device_add_child_ordered(dev, order, name, unit));
}

void
bus_generic_driver_added(device_t dev, driver_t *driver)
{
	device_t child;

	/* no DEVICE_IDENTIFY: LinuxKPI drivers have no identify method */
	TAILQ_FOREACH(child, &dev->children, link)
		if (child->state == DS_NOTPRESENT)
			device_probe_and_attach(child);
}

int
bus_generic_detach(device_t dev)
{
	int error;

	if ((error = bus_detach_children(dev)) != 0)
		return (error);
	return (device_delete_children(dev));
}

int
bus_generic_shutdown(device_t dev)
{
	device_t child;

	TAILQ_FOREACH(child, &dev->children, link)
		device_shutdown(child);
	return (0);
}

int
bus_generic_suspend(device_t dev)
{
	device_t child, child2;
	int error;

	TAILQ_FOREACH(child, &dev->children, link) {
		if (child->state < DS_ATTACHED || (child->flags & DF_SUSPENDED))
			continue;
		if ((error = DEVICE_SUSPEND(child)) != 0) {
			/* undo the ones already suspended */
			for (child2 = TAILQ_FIRST(&dev->children); child2 != NULL && child2 != child;
			    child2 = TAILQ_NEXT(child2, link))
				if (child2->flags & DF_SUSPENDED) {
					DEVICE_RESUME(child2);
					child2->flags &= ~DF_SUSPENDED;
				}
			return (error);
		}
		child->flags |= DF_SUSPENDED;
	}
	return (0);
}

int
bus_generic_resume(device_t dev)
{
	device_t child;

	TAILQ_FOREACH(child, &dev->children, link)
		if (child->flags & DF_SUSPENDED) {
			DEVICE_RESUME(child);
			child->flags &= ~DF_SUSPENDED;
		}
	return (0);
}

int
bus_generic_read_ivar(device_t dev, device_t child, int index, uintptr_t *result)
{
	return (ENOENT);
}

struct resource *
bus_generic_alloc_resource(device_t dev, device_t child, int type, int rid, rman_res_t start, rman_res_t end,
    rman_res_t count, u_int flags)
{
	if (dev->parent != NULL)
		return (BUS_ALLOC_RESOURCE(dev->parent, child, type, rid, start, end, count, flags));
	return (NULL);
}

int
bus_generic_release_resource(device_t dev, device_t child, struct resource *r)
{
	if (dev->parent != NULL)
		return (BUS_RELEASE_RESOURCE(dev->parent, child, r));
	return (EINVAL);
}

int
bus_generic_activate_resource(device_t dev, device_t child, struct resource *r)
{
	if (dev->parent != NULL)
		return (BUS_ACTIVATE_RESOURCE(dev->parent, child, r));
	return (EINVAL);
}

int
bus_generic_deactivate_resource(device_t dev, device_t child, struct resource *r)
{
	if (dev->parent != NULL)
		return (BUS_DEACTIVATE_RESOURCE(dev->parent, child, r));
	return (EINVAL);
}

int
bus_generic_translate_resource(device_t dev, int type, rman_res_t start, rman_res_t *newstart)
{
	if (dev->parent != NULL)
		return (BUS_TRANSLATE_RESOURCE(dev->parent, type, start, newstart));
	*newstart = start;
	return (0);
}

int
bus_generic_setup_intr(device_t dev, device_t child, struct resource *irq, int flags, driver_filter_t *filter,
    driver_intr_t *intr, void *arg, void **cookiep)
{
	if (dev->parent != NULL)
		return (BUS_SETUP_INTR(dev->parent, child, irq, flags, filter, intr, arg, cookiep));
	return (EINVAL);
}

int
bus_generic_teardown_intr(device_t dev, device_t child, struct resource *irq, void *cookie)
{
	if (dev->parent != NULL)
		return (BUS_TEARDOWN_INTR(dev->parent, child, irq, cookie));
	return (EINVAL);
}

int
bus_generic_bind_intr(device_t dev, device_t child, struct resource *irq, int cpu)
{
	if (dev->parent != NULL)
		return (BUS_BIND_INTR(dev->parent, child, irq, cpu));
	return (EINVAL);
}

int
bus_generic_child_present(device_t dev, device_t child)
{
	return (BUS_CHILD_PRESENT(device_get_parent(dev), dev));
}

bus_dma_tag_t
bus_generic_get_dma_tag(device_t dev, device_t child)
{
	if (dev->parent != NULL)
		return (BUS_GET_DMA_TAG(dev->parent, child));
	return (NULL);
}

int
bus_generic_get_domain(device_t dev, device_t child, int *domain)
{
	if (dev->parent != NULL)
		return (BUS_GET_DOMAIN(dev->parent, dev, domain));
	return (ENOENT);
}

/*
 * Front ends (the bus_*() calls drivers make on themselves)
 */

struct resource *
(bus_alloc_resource)(device_t dev, int type, int rid, rman_res_t start, rman_res_t end, rman_res_t count,
    u_int flags)
{
	if (dev->parent == NULL)
		return (NULL);
	return (BUS_ALLOC_RESOURCE(dev->parent, dev, type, rid, start, end, count, flags));
}

int
(bus_activate_resource)(device_t dev, struct resource *r)
{
	if (dev->parent == NULL)
		return (EINVAL);
	return (BUS_ACTIVATE_RESOURCE(dev->parent, dev, r));
}

int
(bus_deactivate_resource)(device_t dev, struct resource *r)
{
	if (dev->parent == NULL)
		return (EINVAL);
	return (BUS_DEACTIVATE_RESOURCE(dev->parent, dev, r));
}

int
bus_deactivate_resource_old(device_t dev, int type, int rid, struct resource *r)
{
	return ((bus_deactivate_resource)(dev, r));
}

int
(bus_release_resource)(device_t dev, struct resource *r)
{
	if (dev->parent == NULL)
		return (EINVAL);
	return (BUS_RELEASE_RESOURCE(dev->parent, dev, r));
}

int
bus_release_resource_old(device_t dev, int type, int rid, struct resource *r)
{
	KASSERT(rman_get_type(r) == type && rman_get_rid(r) == rid, ("%s: type/rid mismatch", __func__));
	return ((bus_release_resource)(dev, r));
}

int
bus_translate_resource(device_t dev, int type, rman_res_t start, rman_res_t *newstart)
{
	if (dev->parent == NULL)
		return (EINVAL);
	return (BUS_TRANSLATE_RESOURCE(dev->parent, type, start, newstart));
}

int
bus_setup_intr(device_t dev, struct resource *r, int flags, driver_filter_t filter, driver_intr_t handler,
    void *arg, void **cookiep)
{
	if (dev->parent == NULL)
		return (EINVAL);
	return (BUS_SETUP_INTR(dev->parent, dev, r, flags, filter, handler, arg, cookiep));
}

int
bus_teardown_intr(device_t dev, struct resource *r, void *cookie)
{
	if (dev->parent == NULL)
		return (EINVAL);
	return (BUS_TEARDOWN_INTR(dev->parent, dev, r, cookie));
}

int
bus_bind_intr(device_t dev, struct resource *r, int cpu)
{
	if (dev->parent == NULL)
		return (EINVAL);
	return (BUS_BIND_INTR(dev->parent, dev, r, cpu));
}

int
bus_child_present(device_t child)
{
	return (BUS_CHILD_PRESENT(device_get_parent(child), child));
}

bus_dma_tag_t
bus_get_dma_tag(device_t dev)
{
	device_t parent = device_get_parent(dev);

	if (parent == NULL)
		return (NULL);
	return (BUS_GET_DMA_TAG(parent, dev));
}

int
bus_get_domain(device_t dev, int *domain)
{
	if (dev->parent == NULL)
		return (ENOENT);
	return (BUS_GET_DOMAIN(dev->parent, dev, domain));
}

/*
 * Resources. FreeBSD keeps them in rmans under nexus; here the PCI bus is the only source, so a resource is a
 * small record that knows its device, and the rman_get_* accessors read it.
 */

struct xg_pci_dinfo;

struct xg_pci_res {
	struct resource	r;		/* first: drivers hold struct resource * */
	struct xg_pci_dinfo *di;	/* the PCI function it belongs to */
	device_t	dev;
	int		type;
	int		rid;
	rman_res_t	start;
	rman_res_t	end;
	u_int		flags;
	void		*map;		/* IOMemoryMap of an active memory BAR */
	void		*virt;
};

static inline struct xg_pci_res *
xg_pci_res(const struct resource *r)
{
	return (__containerof(__DECONST(struct resource *, r), struct xg_pci_res, r));
}

rman_res_t
rman_get_start(const struct resource *r)
{
	return (xg_pci_res(r)->start);
}

rman_res_t
rman_get_end(const struct resource *r)
{
	return (xg_pci_res(r)->end);
}

rman_res_t
rman_get_size(const struct resource *r)
{
	return (xg_pci_res(r)->end - xg_pci_res(r)->start + 1);
}

u_int
rman_get_flags(const struct resource *r)
{
	return (xg_pci_res(r)->flags);
}

int
rman_get_rid(const struct resource *r)
{
	return (xg_pci_res(r)->rid);
}

int
rman_get_type(const struct resource *r)
{
	return (xg_pci_res(r)->type);
}

device_t
rman_get_device(const struct resource *r)
{
	return (xg_pci_res(r)->dev);
}

void *
rman_get_virtual(const struct resource *r)
{
	return (xg_pci_res(r)->virt);
}

bus_space_tag_t
rman_get_bustag(const struct resource *r)
{
	return (r->r_bustag);
}

bus_space_handle_t
rman_get_bushandle(const struct resource *r)
{
	return (r->r_bushandle);
}

static struct xg_pci_res *
xg_pci_res_new(struct xg_pci_dinfo *di, device_t dev, int type, int rid, rman_res_t start, rman_res_t end,
    u_int flags)
{
	struct xg_pci_res *xr;

	xr = malloc(sizeof(*xr), M_XGBUS, M_WAITOK | M_ZERO);
	xr->r.__r_i = (struct resource_i *)xr;
	xr->di = di;
	xr->dev = dev;
	xr->type = type;
	xr->rid = rid;
	xr->start = start;
	xr->end = end;
	xr->flags = (flags & ~RF_ACTIVE) | RF_ALLOCATED;
	return (xr);
}

static struct resource_list_entry *
xg_pci_rle_find(struct resource_list *rl, int type, int rid)
{
	struct resource_list_entry *rle;

	STAILQ_FOREACH(rle, rl, link)
		if (rle->type == type && rle->rid == rid)
			return (rle);
	return (NULL);
}

static struct resource_list_entry *
xg_pci_rle_add(struct resource_list *rl, int type, int rid, rman_res_t start, rman_res_t end, rman_res_t count)
{
	struct resource_list_entry *rle;

	if ((rle = xg_pci_rle_find(rl, type, rid)) == NULL) {
		rle = malloc(sizeof(*rle), M_XGBUS, M_WAITOK | M_ZERO);
		STAILQ_INSERT_TAIL(rl, rle, link);
		rle->type = type;
		rle->rid = rid;
	}
	rle->start = start;
	rle->end = end;
	rle->count = count;
	return (rle);
}

static void
xg_pci_rle_delete(struct resource_list *rl, int type, int rid)
{
	struct resource_list_entry *rle;

	if ((rle = xg_pci_rle_find(rl, type, rid)) != NULL) {
		STAILQ_REMOVE(rl, rle, resource_list_entry, link);
		free(rle, M_XGBUS);
	}
}

/*
 * bus_dma tags (x86 busdma_bounce.c without bounce pages). Bus address == physical address, which holds while the
 * IOMMU is off (the IOMapper disabled); a buffer outside the tag's window fails instead of bouncing.
 */

#define XG_PCI_DMA_SEGS	256

struct bus_dma_tag {
	struct bus_dma_tag_common common;
	int		map_count;
	bus_dma_segment_t *segments;	/* for loads that pass no segment array */
};

struct bus_dmamap {
	int		loaded;
};

static struct bus_dma_impl xg_pci_dma_impl;
static bus_dma_tag_t xg_pci_dmat;	/* the PCI bus tag: segments never cross 4 GB, like FreeBSD's pci(4) */

int
bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment, bus_addr_t boundary, bus_addr_t lowaddr,
    bus_addr_t highaddr, bus_dma_filter_t *filter, void *filterarg, bus_size_t maxsize, int nsegments,
    bus_size_t maxsegsz, int flags, bus_dma_lock_t *lockfunc, void *lockfuncarg, bus_dma_tag_t *dmat)
{
	struct bus_dma_tag_common *pc = parent != NULL ? &parent->common : NULL;
	struct bus_dma_tag *t;
	u_int nseg;

	*dmat = NULL;
	/* x86 dropped filter functions; same answer here */
	if (filter != NULL || filterarg != NULL)
		return (EINVAL);
	if (alignment == 0 || !powerof2(alignment) || (boundary != 0 && !powerof2(boundary)))
		return (EINVAL);
	if (boundary != 0 && boundary < maxsegsz)
		maxsegsz = boundary;
	if (maxsegsz == 0)
		return (EINVAL);
	t = malloc(sizeof(*t), M_DEVBUF, (flags & BUS_DMA_NOWAIT) ? M_NOWAIT | M_ZERO : M_WAITOK | M_ZERO);
	if (t == NULL)
		return (ENOMEM);
	t->common.impl = &xg_pci_dma_impl;
	t->common.alignment = alignment;
	t->common.boundary = boundary;
	t->common.lowaddr = trunc_page((vm_paddr_t)lowaddr) + (PAGE_SIZE - 1);
	t->common.highaddr = trunc_page((vm_paddr_t)highaddr) + (PAGE_SIZE - 1);
	t->common.maxsize = maxsize;
	t->common.nsegments = nsegments;
	t->common.maxsegsz = maxsegsz;
	t->common.flags = flags;
	t->common.lockfunc = lockfunc;
	t->common.lockfuncarg = lockfuncarg;
	t->common.domain = 0;
	if (pc != NULL) {
		/* a child tag can only narrow its parent's constraints */
		t->common.lowaddr = MIN(pc->lowaddr, t->common.lowaddr);
		t->common.highaddr = MAX(pc->highaddr, t->common.highaddr);
		if (t->common.boundary == 0)
			t->common.boundary = pc->boundary;
		else if (pc->boundary != 0)
			t->common.boundary = MIN(pc->boundary, t->common.boundary);
		if (t->common.boundary != 0 && t->common.boundary < t->common.maxsegsz)
			t->common.maxsegsz = t->common.boundary;
		t->common.alignment = MAX(pc->alignment, t->common.alignment);
		t->common.domain = pc->domain;
	}
	nseg = MIN(t->common.nsegments, XG_PCI_DMA_SEGS);
	t->segments = malloc(MAX(nseg, 1) * sizeof(bus_dma_segment_t), M_DEVBUF,
	    (flags & BUS_DMA_NOWAIT) ? M_NOWAIT : M_WAITOK);
	if (t->segments == NULL) {
		free(t, M_DEVBUF);
		return (ENOMEM);
	}
	*dmat = t;
	return (0);
}

int
bus_dma_tag_destroy(bus_dma_tag_t dmat)
{
	if (dmat == NULL)
		return (0);
	if (dmat->map_count != 0)
		return (EBUSY);
	free(dmat->segments, M_DEVBUF);
	free(dmat, M_DEVBUF);
	return (0);
}

static int
xg_pci_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment, bus_addr_t boundary, bus_addr_t lowaddr,
    bus_addr_t highaddr, bus_size_t maxsize, int nsegments, bus_size_t maxsegsz, int flags,
    bus_dma_lock_t *lockfunc, void *lockfuncarg, bus_dma_tag_t *dmat)
{
	return (bus_dma_tag_create(parent, alignment, boundary, lowaddr, highaddr, NULL, NULL, maxsize, nsegments,
	    maxsegsz, flags, lockfunc, lockfuncarg, dmat));
}

static int
xg_pci_dma_set_domain(bus_dma_tag_t dmat)
{
	return (0);
}

static bool
xg_pci_dma_reachable(bus_dma_tag_t dmat, vm_paddr_t paddr, bus_size_t len)
{
	if (len == 0)
		return (true);
	if (paddr + len - 1 > dmat->common.lowaddr || paddr + len - 1 < paddr)
		return (false);
	return (dmat->common.alignment <= 1 || (paddr & (dmat->common.alignment - 1)) == 0);
}

static bool
xg_pci_dma_id_mapped(bus_dma_tag_t dmat, vm_paddr_t buf, bus_size_t buflen)
{
	return (xg_pci_dma_reachable(dmat, buf, buflen));
}

static int
xg_pci_dma_map_create(bus_dma_tag_t dmat, int flags, bus_dmamap_t *mapp)
{
	*mapp = malloc(sizeof(**mapp), M_DEVBUF, (flags & BUS_DMA_NOWAIT) ? M_NOWAIT | M_ZERO : M_WAITOK | M_ZERO);
	if (*mapp == NULL)
		return (ENOMEM);
	atomic_add_int((volatile u_int *)&dmat->map_count, 1);
	return (0);
}

static int
xg_pci_dma_map_destroy(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	if (map == NULL)
		return (0);
	if (map->loaded)
		return (EBUSY);
	free(map, M_DEVBUF);
	atomic_subtract_int((volatile u_int *)&dmat->map_count, 1);
	return (0);
}

static int
xg_pci_dma_mem_alloc(bus_dma_tag_t dmat, void **vaddr, int flags, bus_dmamap_t *mapp)
{
	int mflags = (flags & BUS_DMA_NOWAIT) ? M_NOWAIT : M_WAITOK;

	if (flags & BUS_DMA_ZERO)
		mflags |= M_ZERO;
	*vaddr = contigmalloc(dmat->common.maxsize, M_DEVBUF, mflags, 0ul, dmat->common.lowaddr,
	    MAX(dmat->common.alignment, 1), dmat->common.boundary);
	if (*vaddr == NULL)
		return (ENOMEM);
	if (xg_pci_dma_map_create(dmat, flags, mapp) != 0) {
		contigfree(*vaddr, dmat->common.maxsize, M_DEVBUF);
		*vaddr = NULL;
		return (ENOMEM);
	}
	return (0);
}

static void
xg_pci_dma_mem_free(bus_dma_tag_t dmat, void *vaddr, bus_dmamap_t map)
{
	contigfree(vaddr, dmat->common.maxsize, M_DEVBUF);
	if (map != NULL)
		map->loaded = 0;
	xg_pci_dma_map_destroy(dmat, map);
}

/* append [curaddr, curaddr + sgsize) to the segment list, merging with the last one when the tag allows */
static bus_size_t
xg_pci_dma_add_seg(bus_dma_tag_t dmat, vm_paddr_t curaddr, bus_size_t sgsize, bus_dma_segment_t *segs, int *segp,
    int nsegs)
{
	bus_addr_t baddr, bmask = ~(dmat->common.boundary - 1);
	int seg = *segp;

	if (dmat->common.boundary > 0) {
		baddr = (curaddr + dmat->common.boundary) & bmask;
		if (sgsize > baddr - curaddr)
			sgsize = baddr - curaddr;
	}
	if (seg >= 0 && curaddr == segs[seg].ds_addr + segs[seg].ds_len &&
	    segs[seg].ds_len + sgsize <= dmat->common.maxsegsz &&
	    (dmat->common.boundary == 0 || (segs[seg].ds_addr & bmask) == (curaddr & bmask)))
		segs[seg].ds_len += sgsize;
	else {
		if (++seg >= nsegs)
			return (0);
		segs[seg].ds_addr = curaddr;
		segs[seg].ds_len = sgsize;
	}
	*segp = seg;
	return (sgsize);
}

static int
xg_pci_dma_segs(bus_dma_tag_t dmat, bus_dma_segment_t **segs)
{
	if (*segs == NULL) {
		*segs = dmat->segments;
		return (MIN(dmat->common.nsegments, XG_PCI_DMA_SEGS));
	}
	return (dmat->common.nsegments);
}

static int
xg_pci_dma_load_phys(bus_dma_tag_t dmat, bus_dmamap_t map, vm_paddr_t buf, bus_size_t buflen, int flags,
    bus_dma_segment_t *segs, int *segp)
{
	bus_size_t sgsize;
	int nsegs = xg_pci_dma_segs(dmat, &segs);

	if (!xg_pci_dma_reachable(dmat, buf, buflen))
		return (ENOMEM);	/* would need a bounce page */
	while (buflen > 0) {
		sgsize = MIN(buflen, dmat->common.maxsegsz);
		if ((sgsize = xg_pci_dma_add_seg(dmat, buf, sgsize, segs, segp, nsegs)) == 0)
			return (EFBIG);
		buf += sgsize;
		buflen -= sgsize;
	}
	if (map != NULL)
		map->loaded = 1;
	return (0);
}

/* only bus_dmamap_load_ma() in subr_bus_dma.c loads page arrays, and that is not part of the XNU build */
static int
xg_pci_dma_load_ma(bus_dma_tag_t dmat, bus_dmamap_t map, struct vm_page **ma, bus_size_t tlen, int ma_offs,
    int flags, bus_dma_segment_t *segs, int *segp)
{
	return (EOPNOTSUPP);
}

static int
xg_pci_dma_load_buffer(bus_dma_tag_t dmat, bus_dmamap_t map, void *buf, bus_size_t buflen, struct pmap *pmap,
    int flags, bus_dma_segment_t *segs, int *segp)
{
	vm_offset_t va = (vm_offset_t)buf;
	bus_size_t len;
	int error;

	/* kernel buffers only: there are no user pmaps on the XNU side, so pmap is not looked at */
	while (buflen > 0) {
		len = MIN(PAGE_SIZE - (va & PAGE_MASK), buflen);
		if ((error = xg_pci_dma_load_phys(dmat, map, pmap_kextract(va), len, flags, segs, segp)) != 0)
			return (error);
		va += len;
		buflen -= len;
	}
	return (0);
}

static void
xg_pci_dma_map_waitok(bus_dma_tag_t dmat, bus_dmamap_t map, struct memdesc *mem, bus_dmamap_callback_t *callback,
    void *callback_arg)
{
	/* loads never defer (no bounce pages to wait for), so nothing to remember */
}

static bus_dma_segment_t *
xg_pci_dma_map_complete(bus_dma_tag_t dmat, bus_dmamap_t map, bus_dma_segment_t *segs, int nsegs, int error)
{
	return (segs != NULL ? segs : dmat->segments);
}

static void
xg_pci_dma_map_unload(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	map->loaded = 0;
}

static void
xg_pci_dma_map_sync(bus_dma_tag_t dmat, bus_dmamap_t map, bus_dmasync_op_t op)
{
	/* x86 DMA is cache coherent and nothing bounces: only order the CPU's stores before the device reads */
	if (op & BUS_DMASYNC_PREWRITE)
		atomic_thread_fence_rel();
	if (op & BUS_DMASYNC_POSTREAD)
		atomic_thread_fence_acq();
}

static struct bus_dma_impl xg_pci_dma_impl = {
	.tag_create = xg_pci_dma_tag_create,
	.tag_destroy = bus_dma_tag_destroy,
	.tag_set_domain = xg_pci_dma_set_domain,
	.id_mapped = xg_pci_dma_id_mapped,
	.map_create = xg_pci_dma_map_create,
	.map_destroy = xg_pci_dma_map_destroy,
	.mem_alloc = xg_pci_dma_mem_alloc,
	.mem_free = xg_pci_dma_mem_free,
	.load_ma = xg_pci_dma_load_ma,
	.load_phys = xg_pci_dma_load_phys,
	.load_buffer = xg_pci_dma_load_buffer,
	.map_waitok = xg_pci_dma_map_waitok,
	.map_complete = xg_pci_dma_map_complete,
	.map_unload = xg_pci_dma_map_unload,
	.map_sync = xg_pci_dma_map_sync,
};

/*
 * The PCI bus (sys/dev/pci/pci.c), on an IOPCIDevice per function. BARs are the ones firmware and IOPCIFamily
 * assigned: this code never sizes or moves a BAR and never toggles decoding while it looks.
 */

struct xg_pci_dinfo {
	struct pci_devinfo pd;		/* first: LinuxKPI casts the ivars to struct pci_devinfo */
	xg_pci_xnu_t	*xh;
};

struct xg_pci_softc {
	u_int		domain;
	u_int		busno;
};

/* an installed handler; the cookie bus_setup_intr hands out */
struct xg_pci_intr {
	int		irq;
	void		*xcookie;
};

static struct xg_pci_irq {
	xg_pci_xnu_t	*xh;		/* NULL: free */
	int		index;		/* IOKit interrupt index */
	struct xg_pci_intr *ih;
} xg_pci_irqs[XG_PCI_NIRQ];

static device_t xg_pci_root, xg_pci_hostb;

static struct xg_pci_dinfo *
xg_pci_di(device_t dev)
{
	return ((struct xg_pci_dinfo *)device_get_ivars(dev));
}

/* the bus's own child on the path to dev (vgapci for drmn), or NULL */
static device_t
xg_pci_direct_child(device_t bus, device_t dev)
{
	while (dev != NULL && dev->parent != bus)
		dev = dev->parent;
	return (dev);
}

static int
xg_pci_irq_get(xg_pci_xnu_t *xh, int index, int prefer, bool msi)
{
	int lo = msi ? (int)first_msi_irq : 1;
	int hi = msi ? (int)(first_msi_irq + num_msi_irqs) : (int)first_msi_irq;
	int irq = -1;

	mtx_lock(&xg_pci_irq_mtx);
	if (prefer >= lo && prefer < hi && xg_pci_irqs[prefer].xh == NULL)
		irq = prefer;
	for (int i = lo; irq < 0 && i < hi && i < XG_PCI_NIRQ; i++)
		if (xg_pci_irqs[i].xh == NULL)
			irq = i;
	if (irq >= 0) {
		xg_pci_irqs[irq].xh = xh;
		xg_pci_irqs[irq].index = index;
		xg_pci_irqs[irq].ih = NULL;
	}
	mtx_unlock(&xg_pci_irq_mtx);
	return (irq);
}

static void
xg_pci_irq_put(int irq)
{
	if (irq <= 0 || irq >= XG_PCI_NIRQ)
		return;
	mtx_lock(&xg_pci_irq_mtx);
	xg_pci_irqs[irq].xh = NULL;
	mtx_unlock(&xg_pci_irq_mtx);
}

static uint32_t
xg_pci_read_config_method(device_t dev, device_t child, int reg, int width)
{
	struct xg_pci_dinfo *di = xg_pci_di(child);

	if (reg < 0 || reg > PCIE_REGMAX || (width != 1 && width != 2 && width != 4))
		return (~0u);
	return (xg_pci_xnu_cfg_read(di->xh, reg, width));
}

static void
xg_pci_write_config_method(device_t dev, device_t child, int reg, uint32_t val, int width)
{
	struct xg_pci_dinfo *di = xg_pci_di(child);

	if (reg < 0 || reg > PCIE_REGMAX || (width != 1 && width != 2 && width != 4))
		return;
	xg_pci_xnu_cfg_write(di->xh, reg, val, width);
}

static int
xg_pci_read_ivar_method(device_t dev, device_t child, int which, uintptr_t *result)
{
	struct xg_pci_dinfo *di = xg_pci_di(child);
	pcicfgregs *cfg;

	if (di == NULL)
		return (ENOENT);
	cfg = &di->pd.cfg;
	switch (which) {
	case PCI_IVAR_ETHADDR:
		*((uint8_t **)result) = NULL;
		return (EINVAL);
	case PCI_IVAR_SUBVENDOR:	*result = cfg->subvendor; break;
	case PCI_IVAR_SUBDEVICE:	*result = cfg->subdevice; break;
	case PCI_IVAR_VENDOR:		*result = cfg->vendor; break;
	case PCI_IVAR_DEVICE:		*result = cfg->device; break;
	case PCI_IVAR_DEVID:		*result = (cfg->device << 16) | cfg->vendor; break;
	case PCI_IVAR_CLASS:		*result = cfg->baseclass; break;
	case PCI_IVAR_SUBCLASS:		*result = cfg->subclass; break;
	case PCI_IVAR_PROGIF:		*result = cfg->progif; break;
	case PCI_IVAR_REVID:		*result = cfg->revid; break;
	case PCI_IVAR_INTPIN:		*result = cfg->intpin; break;
	case PCI_IVAR_IRQ:		*result = cfg->intline; break;
	case PCI_IVAR_DOMAIN:		*result = cfg->domain; break;
	case PCI_IVAR_BUS:		*result = cfg->bus; break;
	case PCI_IVAR_SLOT:		*result = cfg->slot; break;
	case PCI_IVAR_FUNCTION:		*result = cfg->func; break;
	case PCI_IVAR_CMDREG:		*result = cfg->cmdreg; break;
	case PCI_IVAR_CACHELNSZ:	*result = cfg->cachelnsz; break;
	case PCI_IVAR_MINGNT:		*result = cfg->mingnt; break;
	case PCI_IVAR_MAXLAT:		*result = cfg->maxlat; break;
	case PCI_IVAR_LATTIMER:		*result = cfg->lattimer; break;
	default:
		return (ENOENT);
	}
	return (0);
}

static void
xg_pci_command_bit(device_t dev, device_t child, uint16_t bit, bool set)
{
	uint16_t cmd = (uint16_t)PCI_READ_CONFIG(dev, child, PCIR_COMMAND, 2);

	cmd = set ? (cmd | bit) : (cmd & ~bit);
	PCI_WRITE_CONFIG(dev, child, PCIR_COMMAND, cmd, 2);
}

static int
xg_pci_enable_busmaster_method(device_t dev, device_t child)
{
	xg_pci_command_bit(dev, child, PCIM_CMD_BUSMASTEREN, true);
	return (0);
}

static int
xg_pci_disable_busmaster_method(device_t dev, device_t child)
{
	xg_pci_command_bit(dev, child, PCIM_CMD_BUSMASTEREN, false);
	return (0);
}

static int
xg_pci_io_bit(int space, uint16_t *bit)
{
	switch (space) {
	case SYS_RES_IOPORT:
		*bit = PCIM_CMD_PORTEN;
		return (0);
	case SYS_RES_MEMORY:
		*bit = PCIM_CMD_MEMEN;
		return (0);
	}
	return (EINVAL);
}

static int
xg_pci_enable_io_method(device_t dev, device_t child, int space)
{
	uint16_t bit;

	if (xg_pci_io_bit(space, &bit) != 0)
		return (EINVAL);
	xg_pci_command_bit(dev, child, bit, true);
	return (0);
}

static int
xg_pci_disable_io_method(device_t dev, device_t child, int space)
{
	uint16_t bit;

	if (xg_pci_io_bit(space, &bit) != 0)
		return (EINVAL);
	xg_pci_command_bit(dev, child, bit, false);
	return (0);
}

static int
xg_pci_get_powerstate_method(device_t dev, device_t child)
{
	pcicfgregs *cfg = &xg_pci_di(child)->pd.cfg;
	uint16_t status;

	if (cfg->pp.pp_location == 0)
		return (PCI_POWERSTATE_D0);
	status = (uint16_t)PCI_READ_CONFIG(dev, child, cfg->pp.pp_location + PCIR_POWER_STATUS, 2);
	switch (status & PCIM_PSTAT_DMASK) {
	case PCIM_PSTAT_D0:	return (PCI_POWERSTATE_D0);
	case PCIM_PSTAT_D1:	return (PCI_POWERSTATE_D1);
	case PCIM_PSTAT_D2:	return (PCI_POWERSTATE_D2);
	case PCIM_PSTAT_D3:	return (PCI_POWERSTATE_D3);
	}
	return (PCI_POWERSTATE_UNKNOWN);
}

static int
xg_pci_set_powerstate_method(device_t dev, device_t child, int state)
{
	pcicfgregs *cfg = &xg_pci_di(child)->pd.cfg;
	int oldstate, highest, delay;
	uint16_t status;

	if (cfg->pp.pp_location == 0)
		return (EOPNOTSUPP);
	/* no write for no change; some devices misbehave on D3 -> D3 */
	oldstate = PCI_GET_POWERSTATE(dev, child);
	if (oldstate == state)
		return (0);
	highest = (oldstate > state) ? oldstate : state;
	delay = highest == PCI_POWERSTATE_D3 ? 10000 : highest == PCI_POWERSTATE_D2 ? 200 : 0;
	status = (uint16_t)(PCI_READ_CONFIG(dev, child, cfg->pp.pp_location + PCIR_POWER_STATUS, 2) &
	    ~PCIM_PSTAT_DMASK);
	switch (state) {
	case PCI_POWERSTATE_D0:
		status |= PCIM_PSTAT_D0;
		break;
	case PCI_POWERSTATE_D1:
		if ((cfg->pp.pp_cap & PCIM_PCAP_D1SUPP) == 0)
			return (EOPNOTSUPP);
		status |= PCIM_PSTAT_D1;
		break;
	case PCI_POWERSTATE_D2:
		if ((cfg->pp.pp_cap & PCIM_PCAP_D2SUPP) == 0)
			return (EOPNOTSUPP);
		status |= PCIM_PSTAT_D2;
		break;
	case PCI_POWERSTATE_D3:
		status |= PCIM_PSTAT_D3;
		break;
	default:
		return (EINVAL);
	}
	PCI_WRITE_CONFIG(dev, child, cfg->pp.pp_location + PCIR_POWER_STATUS, status, 2);
	if (delay)
		DELAY(delay);
	return (0);
}

static int
xg_pci_get_id_method(device_t dev, device_t child, enum pci_id_type type, uintptr_t *id)
{
	pcicfgregs *cfg = &xg_pci_di(child)->pd.cfg;

	if (type != PCI_ID_RID)
		return (ENXIO);
	*id = PCI_RID(cfg->bus, cfg->slot, cfg->func);
	return (0);
}

static int
xg_pci_msi_count_method(device_t dev, device_t child)
{
	struct pcicfg_msi *msi = &xg_pci_di(child)->pd.cfg.msi;

	if (msi->msi_location == 0)
		return (0);
	return (PCI_MSI_MSGNUM(PCI_READ_CONFIG(dev, child, msi->msi_location + PCIR_MSI_CTRL, 2)));
}

static int
xg_pci_msix_count_method(device_t dev, device_t child)
{
	struct pcicfg_msix *msix = &xg_pci_di(child)->pd.cfg.msix;

	if (msix->msix_location == 0)
		return (0);
	return (PCI_MSIX_MSGNUM(PCI_READ_CONFIG(dev, child, msix->msix_location + PCIR_MSIX_CTRL, 2)));
}

/*
 * MSI and MSI-X: IOPCIFamily owns the capability registers (address, data, enable). This side keeps FreeBSD's
 * bookkeeping and hands out SYS_RES_IRQ rids 1..n, each tied to one IOKit message interrupt index.
 */
static int
xg_pci_alloc_msg(device_t child, int *count, bool msix)
{
	struct xg_pci_dinfo *di = xg_pci_di(child);
	pcicfgregs *cfg = &di->pd.cfg;
	struct resource_list_entry *rle;
	int idx[32], n, actual, irq;

	if (*count < 1)
		return (EINVAL);
	rle = xg_pci_rle_find(&di->pd.resources, SYS_RES_IRQ, 0);
	if (rle != NULL && rle->res != NULL)
		return (ENXIO);
	if (cfg->msi.msi_alloc != 0 || cfg->msix.msix_alloc != 0)
		return (ENXIO);
	if (msix) {
		if (cfg->msix.msix_location == 0)
			return (ENODEV);
		actual = MIN(*count, (int)PCI_MSIX_MSGNUM(cfg->msix.msix_ctrl));
	} else {
		if (cfg->msi.msi_location == 0)
			return (ENODEV);
		actual = MIN(*count, (int)PCI_MSI_MSGNUM(cfg->msi.msi_ctrl));
		if (!powerof2(actual))
			return (EINVAL);
	}
	actual = MIN(actual, (int)nitems(idx));
	n = xg_pci_xnu_msi_alloc(di->xh, msix, actual, idx, nitems(idx));
	if (n <= 0)
		return (n < 0 ? -n : ENXIO);
	actual = MIN(actual, n);
	/* MSI wants a power of two; MSI-X takes what IOKit gave */
	while (!msix && !powerof2(actual))
		actual--;
	for (int i = 0; i < actual; i++) {
		if ((irq = xg_pci_irq_get(di->xh, idx[i], -1, true)) < 0) {
			for (int j = 0; j < i; j++) {
				rle = xg_pci_rle_find(&di->pd.resources, SYS_RES_IRQ, j + 1);
				xg_pci_irq_put(rle->start);
				xg_pci_rle_delete(&di->pd.resources, SYS_RES_IRQ, j + 1);
			}
			return (ENOSPC);
		}
		xg_pci_rle_add(&di->pd.resources, SYS_RES_IRQ, i + 1, irq, irq, 1);
	}
	if (msix) {
		cfg->msix.msix_alloc = actual;
		cfg->msix.msix_table_len = actual;
	} else {
		cfg->msi.msi_alloc = actual;
		cfg->msi.msi_handlers = 0;
	}
	*count = actual;
	return (0);
}

static int
xg_pci_alloc_msi_method(device_t dev, device_t child, int *count)
{
	return (xg_pci_alloc_msg(child, count, false));
}

static int
xg_pci_alloc_msix_method(device_t dev, device_t child, int *count)
{
	return (xg_pci_alloc_msg(child, count, true));
}

static int
xg_pci_release_msi_method(device_t dev, device_t child)
{
	struct xg_pci_dinfo *di = xg_pci_di(child);
	pcicfgregs *cfg = &di->pd.cfg;
	struct resource_list_entry *rle;
	u_int n;

	n = cfg->msix.msix_alloc != 0 ? cfg->msix.msix_alloc : cfg->msi.msi_alloc;
	if (n == 0)
		return (ENODEV);
	if (cfg->msi.msi_handlers > 0)
		return (EBUSY);
	for (u_int i = 0; i < n; i++) {
		rle = xg_pci_rle_find(&di->pd.resources, SYS_RES_IRQ, i + 1);
		if (rle == NULL || rle->res != NULL)
			return (EBUSY);
	}
	/* the IOKit vectors stay configured; only the rids and irq numbers go back */
	for (u_int i = 0; i < n; i++) {
		rle = xg_pci_rle_find(&di->pd.resources, SYS_RES_IRQ, i + 1);
		xg_pci_irq_put(rle->start);
		xg_pci_rle_delete(&di->pd.resources, SYS_RES_IRQ, i + 1);
	}
	cfg->msix.msix_alloc = 0;
	cfg->msix.msix_table_len = 0;
	cfg->msi.msi_alloc = 0;
	cfg->msi.msi_addr = 0;
	cfg->msi.msi_data = 0;
	return (0);
}

struct pci_map *
pci_find_bar(device_t dev, int reg)
{
	struct pci_devinfo *dinfo = device_get_ivars(dev);
	struct pci_map *pm;

	STAILQ_FOREACH(pm, &dinfo->cfg.maps, pm_link)
		if (pm->pm_reg == reg)
			return (pm);
	return (NULL);
}

/* a BAR's window as IOKit assigned it, reserved but not allocated or mapped */
struct resource *
pci_reserve_map(device_t dev, device_t child, int type, int rid, rman_res_t start, rman_res_t end,
    rman_res_t count, u_int num, u_int flags)
{
	struct xg_pci_dinfo *di = xg_pci_di(child);
	struct resource_list_entry *rle;
	unsigned long long base, len;
	struct xg_pci_res *xr;
	struct pci_map *pm;
	bool mem;

	if ((pm = pci_find_bar(child, rid)) == NULL)
		return (NULL);
	mem = PCI_BAR_MEM(pm->pm_value) || rid == PCIR_BIOS;
	if (type != (mem ? SYS_RES_MEMORY : SYS_RES_IOPORT))
		return (NULL);
	if (xg_pci_xnu_bar(di->xh, rid, &base, &len) != 0 || len == 0) {
		device_printf(child, "rid %#x has no IOKit range\n", rid);
		return (NULL);
	}
	if (!mem)
		base = pm->pm_value & PCIM_BAR_IO_BASE;
	count = (rman_res_t)len * MAX(num, 1u);
	if (mem && (pm->pm_value & PCIM_BAR_MEM_PREFETCH))
		flags |= RF_PREFETCHABLE;
	rle = xg_pci_rle_add(&di->pd.resources, type, rid, base, base + count - 1, count);
	if (rle->res != NULL)
		return (rle->res);
	xr = xg_pci_res_new(di, dev, type, rid, base, base + count - 1, flags);
	rle->res = &xr->r;
	rle->flags = RLE_RESERVED;
	return (rle->res);
}

static int
xg_pci_activate_resource_method(device_t dev, device_t child, struct resource *r)
{
	struct xg_pci_res *xr = xg_pci_res(r);
	unsigned long va;
	int error;

	if (xr->flags & RF_ACTIVE)
		return (0);
	switch (xr->type) {
	case SYS_RES_MEMORY:
		r->r_bustag = X86_BUS_SPACE_MEM;
		if (!(xr->flags & RF_UNMAPPED)) {
			/* uncached, as nexus maps device memory on amd64 */
			if ((error = xg_pci_xnu_map(xr->di->xh, xr->rid, &va, &xr->map)) != 0)
				return (error);
			xr->virt = (void *)va;
			r->r_bushandle = va;
		}
		break;
	case SYS_RES_IOPORT:
		r->r_bustag = X86_BUS_SPACE_IO;
		r->r_bushandle = xr->start;
		break;
	case SYS_RES_IRQ:
		break;
	default:
		return (EINVAL);
	}
	xr->flags |= RF_ACTIVE;
	/* activating a BAR turns its decoding on, as pci_activate_resource does */
	if (xr->type == SYS_RES_MEMORY || xr->type == SYS_RES_IOPORT)
		PCI_ENABLE_IO(dev, xr->di->pd.cfg.dev, xr->type);
	return (0);
}

static int
xg_pci_deactivate_resource_method(device_t dev, device_t child, struct resource *r)
{
	struct xg_pci_res *xr = xg_pci_res(r);

	if (!(xr->flags & RF_ACTIVE))
		return (0);
	if (xr->map != NULL) {
		xg_pci_xnu_unmap(xr->map);
		xr->map = NULL;
	}
	xr->virt = NULL;
	r->r_bushandle = 0;
	xr->flags &= ~RF_ACTIVE;
	return (0);
}

static struct resource *
xg_pci_alloc_resource_method(device_t dev, device_t child, int type, int rid, rman_res_t start, rman_res_t end,
    rman_res_t count, u_int flags)
{
	device_t pchild = xg_pci_direct_child(dev, child);
	struct xg_pci_dinfo *di;
	struct resource_list_entry *rle;
	struct xg_pci_res *xr;
	pcicfgregs *cfg;

	if (pchild == NULL || (di = xg_pci_di(pchild)) == NULL)
		return (NULL);
	cfg = &di->pd.cfg;
	switch (type) {
	case SYS_RES_IRQ:
		if (rid == 0 && (cfg->msi.msi_alloc > 0 || cfg->msix.msix_alloc > 0))
			return (NULL);
		if ((rle = xg_pci_rle_find(&di->pd.resources, type, rid)) == NULL)
			return (NULL);
		if (rle->res != NULL) {
			device_printf(child, "irq rid %d is busy\n", rid);
			return (NULL);
		}
		xr = xg_pci_res_new(di, child, type, rid, rle->start, rle->end, flags);
		rle->res = &xr->r;
		if ((flags & RF_ACTIVE) && bus_activate_resource(child, &xr->r) != 0) {
			rle->res = NULL;
			free(xr, M_XGBUS);
			return (NULL);
		}
		return (&xr->r);
	case SYS_RES_MEMORY:
	case SYS_RES_IOPORT:
		if ((rle = xg_pci_rle_find(&di->pd.resources, type, rid)) == NULL &&
		    pci_reserve_map(dev, pchild, type, rid, start, end, count, 1, flags) == NULL)
			return (NULL);
		if ((rle = xg_pci_rle_find(&di->pd.resources, type, rid)) == NULL || rle->res == NULL)
			return (NULL);
		if (rle->flags & RLE_ALLOCATED)
			return (NULL);
		rle->flags |= RLE_ALLOCATED;
		xr = xg_pci_res(rle->res);
		xr->dev = child;
		if ((flags & RF_ACTIVE) && bus_activate_resource(child, rle->res) != 0) {
			rle->flags &= ~RLE_ALLOCATED;
			xr->dev = dev;
			return (NULL);
		}
		return (rle->res);
	}
	return (NULL);
}

static int
xg_pci_release_resource_method(device_t dev, device_t child, struct resource *r)
{
	struct xg_pci_res *xr = xg_pci_res(r);
	struct resource_list_entry *rle;
	int error;

	rle = xg_pci_rle_find(&xr->di->pd.resources, xr->type, xr->rid);
	if (rle == NULL || rle->res != r)
		return (EINVAL);
	if (xr->type == SYS_RES_IRQ && xr->start < XG_PCI_NIRQ && xg_pci_irqs[xr->start].ih != NULL)
		return (EBUSY);
	if ((xr->flags & RF_ACTIVE) && (error = bus_deactivate_resource(child, r)) != 0)
		return (error);
	if (rle->flags & RLE_RESERVED) {
		/* back to reserved: the BAR window stays known to the bus */
		rle->flags &= ~RLE_ALLOCATED;
		xr->dev = dev;
		return (0);
	}
	rle->res = NULL;
	free(xr, M_XGBUS);
	return (0);
}

/*
 * Interrupts on IOInterruptEventSource. A handler (FreeBSD's ithread part) runs on its own IOWorkLoop thread, so
 * it may take sleep mutexes; a filter runs in primary interrupt context and must not block.
 */
static int
xg_pci_setup_intr_method(device_t dev, device_t child, struct resource *irq, int flags, driver_filter_t *filter,
    driver_intr_t *intr, void *arg, void **cookiep)
{
	struct xg_pci_res *xr = xg_pci_res(irq);
	struct xg_pci_intr *ih;
	struct xg_pci_irq *e;
	int error, n = (int)xr->start;

	if (xr->type != SYS_RES_IRQ || !(xr->flags & RF_ACTIVE) || (filter == NULL && intr == NULL))
		return (EINVAL);
	if (n <= 0 || n >= XG_PCI_NIRQ)
		return (EINVAL);
	ih = malloc(sizeof(*ih), M_XGBUS, M_WAITOK | M_ZERO);
	ih->irq = n;
	mtx_lock(&xg_pci_irq_mtx);
	e = &xg_pci_irqs[n];
	if (e->xh == NULL) {
		error = EINVAL;
	} else if (e->ih != NULL) {
		/* IOKit registers one handler per interrupt index, so no sharing */
		error = EBUSY;
	} else
		error = xg_pci_xnu_intr_setup(e->xh, e->index, filter, intr, arg, &ih->xcookie);
	if (error == 0) {
		e->ih = ih;
		if (xr->rid > 0)
			xr->di->pd.cfg.msi.msi_handlers++;
	}
	mtx_unlock(&xg_pci_irq_mtx);
	if (error != 0) {
		free(ih, M_XGBUS);
		return (error);
	}
	*cookiep = ih;
	return (0);
}

static int
xg_pci_teardown_intr_method(device_t dev, device_t child, struct resource *irq, void *cookie)
{
	struct xg_pci_res *xr = xg_pci_res(irq);
	struct xg_pci_intr *ih = cookie;

	if (ih == NULL || ih->irq != (int)xr->start)
		return (EINVAL);
	mtx_lock(&xg_pci_irq_mtx);
	if (xg_pci_irqs[ih->irq].ih != ih) {
		mtx_unlock(&xg_pci_irq_mtx);
		return (EINVAL);
	}
	/* waits for a running handler; never call this from that handler */
	xg_pci_xnu_intr_teardown(ih->xcookie);
	xg_pci_irqs[ih->irq].ih = NULL;
	if (xr->rid > 0 && xr->di->pd.cfg.msi.msi_handlers > 0)
		xr->di->pd.cfg.msi.msi_handlers--;
	mtx_unlock(&xg_pci_irq_mtx);
	free(ih, M_XGBUS);
	return (0);
}

/* XNU gives a kext no interrupt affinity control */
static int
xg_pci_bind_intr_method(device_t dev, device_t child, struct resource *irq, int cpu)
{
	return (EOPNOTSUPP);
}

/* synchronize_irq(): wait until a handler running on this irq has returned */
void
_intr_drain(int irq)
{
	if (irq <= 0 || irq >= XG_PCI_NIRQ)
		return;
	mtx_lock(&xg_pci_irq_mtx);
	if (xg_pci_irqs[irq].ih != NULL)
		xg_pci_xnu_intr_drain(xg_pci_irqs[irq].ih->xcookie);
	mtx_unlock(&xg_pci_irq_mtx);
}

static int
xg_pci_child_present_method(device_t dev, device_t child)
{
	struct xg_pci_dinfo *di = xg_pci_di(child);

	if (di == NULL)
		return (-1);
	return (xg_pci_xnu_cfg_read(di->xh, PCIR_VENDOR, 2) != 0xffff);
}

static bus_dma_tag_t
xg_pci_get_dma_tag_method(device_t bus, device_t dev)
{
	return (xg_pci_dmat);
}

static int
xg_pci_bus_attach(device_t dev)
{
	return (0);
}

static device_method_t xg_pci_bus_methods[] = {
	DEVMETHOD(device_attach,		xg_pci_bus_attach),
	DEVMETHOD(device_shutdown,		bus_generic_shutdown),
	DEVMETHOD(device_suspend,		bus_generic_suspend),
	DEVMETHOD(device_resume,		bus_generic_resume),
	DEVMETHOD(bus_add_child,		bus_generic_add_child),
	DEVMETHOD(bus_read_ivar,		xg_pci_read_ivar_method),
	DEVMETHOD(bus_alloc_resource,		xg_pci_alloc_resource_method),
	DEVMETHOD(bus_release_resource,		xg_pci_release_resource_method),
	DEVMETHOD(bus_activate_resource,	xg_pci_activate_resource_method),
	DEVMETHOD(bus_deactivate_resource,	xg_pci_deactivate_resource_method),
	DEVMETHOD(bus_setup_intr,		xg_pci_setup_intr_method),
	DEVMETHOD(bus_teardown_intr,		xg_pci_teardown_intr_method),
	DEVMETHOD(bus_bind_intr,		xg_pci_bind_intr_method),
	DEVMETHOD(bus_child_present,		xg_pci_child_present_method),
	DEVMETHOD(bus_get_dma_tag,		xg_pci_get_dma_tag_method),
	DEVMETHOD(pci_read_config,		xg_pci_read_config_method),
	DEVMETHOD(pci_write_config,		xg_pci_write_config_method),
	DEVMETHOD(pci_enable_busmaster,		xg_pci_enable_busmaster_method),
	DEVMETHOD(pci_disable_busmaster,	xg_pci_disable_busmaster_method),
	DEVMETHOD(pci_enable_io,		xg_pci_enable_io_method),
	DEVMETHOD(pci_disable_io,		xg_pci_disable_io_method),
	DEVMETHOD(pci_get_powerstate,		xg_pci_get_powerstate_method),
	DEVMETHOD(pci_set_powerstate,		xg_pci_set_powerstate_method),
	DEVMETHOD(pci_alloc_msi,		xg_pci_alloc_msi_method),
	DEVMETHOD(pci_alloc_msix,		xg_pci_alloc_msix_method),
	DEVMETHOD(pci_release_msi,		xg_pci_release_msi_method),
	DEVMETHOD(pci_msi_count,		xg_pci_msi_count_method),
	DEVMETHOD(pci_msix_count,		xg_pci_msix_count_method),
	DEVMETHOD(pci_get_id,			xg_pci_get_id_method),
	DEVMETHOD_END
};
static DEFINE_CLASS_0(pci, xg_pci_bus_driver, xg_pci_bus_methods, sizeof(struct xg_pci_softc));

/*
 * vgapci (sys/dev/pci/vga_pci.c): the display function's driver; its "drmn" child is where LinuxKPI's drm
 * drivers attach. Everything is passed on to the PCI bus on behalf of the vgapci device itself.
 */

static int
xg_vgapci_attach(device_t dev)
{
	device_add_child(dev, "drmn", DEVICE_UNIT_ANY);
	bus_attach_children(dev);
	return (0);
}

static int
xg_vgapci_read_ivar(device_t dev, device_t child, int which, uintptr_t *result)
{
	return (BUS_READ_IVAR(device_get_parent(dev), dev, which, result));
}

static struct resource *
xg_vgapci_alloc_resource(device_t dev, device_t child, int type, int rid, rman_res_t start, rman_res_t end,
    rman_res_t count, u_int flags)
{
	return ((bus_alloc_resource)(dev, type, rid, start, end, count, flags));
}

static int
xg_vgapci_release_resource(device_t dev, device_t child, struct resource *r)
{
	return ((bus_release_resource)(dev, r));
}

static int
xg_vgapci_setup_intr(device_t dev, device_t child, struct resource *irq, int flags, driver_filter_t *filter,
    driver_intr_t *intr, void *arg, void **cookiep)
{
	return (bus_setup_intr(dev, irq, flags, filter, intr, arg, cookiep));
}

static int
xg_vgapci_teardown_intr(device_t dev, device_t child, struct resource *irq, void *cookie)
{
	return (bus_teardown_intr(dev, irq, cookie));
}

static bus_dma_tag_t
xg_vgapci_get_dma_tag(device_t bus, device_t child)
{
	return (bus_get_dma_tag(bus));
}

static uint32_t
xg_vgapci_read_config(device_t dev, device_t child, int reg, int width)
{
	return (pci_read_config(dev, reg, width));
}

static void
xg_vgapci_write_config(device_t dev, device_t child, int reg, uint32_t val, int width)
{
	pci_write_config(dev, reg, val, width);
}

static int
xg_vgapci_enable_busmaster(device_t dev, device_t child)
{
	return (pci_enable_busmaster(dev));
}

static int
xg_vgapci_disable_busmaster(device_t dev, device_t child)
{
	return (pci_disable_busmaster(dev));
}

static int
xg_vgapci_enable_io(device_t dev, device_t child, int space)
{
	return (pci_enable_io(dev, space));
}

static int
xg_vgapci_disable_io(device_t dev, device_t child, int space)
{
	return (pci_disable_io(dev, space));
}

static int
xg_vgapci_get_powerstate(device_t dev, device_t child)
{
	return (pci_get_powerstate(dev));
}

static int
xg_vgapci_set_powerstate(device_t dev, device_t child, int state)
{
	return (pci_set_powerstate(dev, state));
}

static int
xg_vgapci_alloc_msi(device_t dev, device_t child, int *count)
{
	return (pci_alloc_msi(dev, count));
}

static int
xg_vgapci_alloc_msix(device_t dev, device_t child, int *count)
{
	return (pci_alloc_msix(dev, count));
}

static int
xg_vgapci_release_msi(device_t dev, device_t child)
{
	return (pci_release_msi(dev));
}

static int
xg_vgapci_msi_count(device_t dev, device_t child)
{
	return (pci_msi_count(dev));
}

static int
xg_vgapci_msix_count(device_t dev, device_t child)
{
	return (pci_msix_count(dev));
}

static device_method_t xg_vgapci_methods[] = {
	DEVMETHOD(device_attach,		xg_vgapci_attach),
	DEVMETHOD(device_detach,		bus_generic_detach),
	DEVMETHOD(device_shutdown,		bus_generic_shutdown),
	DEVMETHOD(device_suspend,		bus_generic_suspend),
	DEVMETHOD(device_resume,		bus_generic_resume),
	DEVMETHOD(bus_add_child,		bus_generic_add_child),
	DEVMETHOD(bus_read_ivar,		xg_vgapci_read_ivar),
	DEVMETHOD(bus_alloc_resource,		xg_vgapci_alloc_resource),
	DEVMETHOD(bus_release_resource,		xg_vgapci_release_resource),
	DEVMETHOD(bus_activate_resource,	bus_generic_activate_resource),
	DEVMETHOD(bus_deactivate_resource,	bus_generic_deactivate_resource),
	DEVMETHOD(bus_setup_intr,		xg_vgapci_setup_intr),
	DEVMETHOD(bus_teardown_intr,		xg_vgapci_teardown_intr),
	DEVMETHOD(bus_get_dma_tag,		xg_vgapci_get_dma_tag),
	DEVMETHOD(pci_read_config,		xg_vgapci_read_config),
	DEVMETHOD(pci_write_config,		xg_vgapci_write_config),
	DEVMETHOD(pci_enable_busmaster,		xg_vgapci_enable_busmaster),
	DEVMETHOD(pci_disable_busmaster,	xg_vgapci_disable_busmaster),
	DEVMETHOD(pci_enable_io,		xg_vgapci_enable_io),
	DEVMETHOD(pci_disable_io,		xg_vgapci_disable_io),
	DEVMETHOD(pci_get_powerstate,		xg_vgapci_get_powerstate),
	DEVMETHOD(pci_set_powerstate,		xg_vgapci_set_powerstate),
	DEVMETHOD(pci_alloc_msi,		xg_vgapci_alloc_msi),
	DEVMETHOD(pci_alloc_msix,		xg_vgapci_alloc_msix),
	DEVMETHOD(pci_release_msi,		xg_vgapci_release_msi),
	DEVMETHOD(pci_msi_count,		xg_vgapci_msi_count),
	DEVMETHOD(pci_msix_count,		xg_vgapci_msix_count),
	DEVMETHOD_END
};
static DEFINE_CLASS_0(vgapci, xg_vgapci_driver, xg_vgapci_methods, 0);

/* root0, the pcib bridges (host and PCI-PCI) and hostb: tree glue with no hardware of their own */

static int
xg_root_child_present(device_t dev, device_t child)
{
	return (-1);
}

static device_method_t xg_root_methods[] = {
	DEVMETHOD(bus_add_child,	bus_generic_add_child),
	DEVMETHOD(bus_child_present,	xg_root_child_present),
	DEVMETHOD_END
};
static DEFINE_CLASS_0(root, xg_root_driver, xg_root_methods, 0);

static device_t xg_pci_new_bus(device_t parent, u_int domain, u_int busno);

static int
xg_pcib_attach(device_t dev)
{
	struct xg_pci_dinfo *di = dev->ivars;

	/* a PCI-PCI bridge hosts the bus behind it; the host bridge gets its root buses on demand */
	if (di != NULL && xg_pci_new_bus(dev, di->pd.cfg.domain, di->pd.cfg.bridge.br_secbus) == NULL)
		return (ENXIO);
	return (0);
}

static device_method_t xg_pcib_methods[] = {
	DEVMETHOD(device_attach,	xg_pcib_attach),
	DEVMETHOD(bus_add_child,	bus_generic_add_child),
	DEVMETHOD_END
};
static DEFINE_CLASS_0(pcib, xg_pcib_driver, xg_pcib_methods, 0);

static int
xg_hostb_attach(device_t dev)
{
	return (0);
}

static device_method_t xg_hostb_methods[] = {
	DEVMETHOD(device_attach,	xg_hostb_attach),
	DEVMETHOD_END
};
static DEFINE_CLASS_0(hostb, xg_hostb_driver, xg_hostb_methods, 0);

/* attach a driver to a device directly, without a probe pass over its parent's devclass */
static int
xg_pci_force_driver(device_t dev, const char *classname, driver_t *driver)
{
	int error;

	if (dev->devclass == NULL && (error = device_set_devclass(dev, classname)) != 0)
		return (error);
	if ((error = device_set_driver(dev, driver)) != 0)
		return (error);
	dev->state = DS_ALIVE;
	return (device_attach(dev));
}

static device_t
xg_pci_new_bus(device_t parent, u_int domain, u_int busno)
{
	struct xg_pci_softc *sc;
	device_t bus;

	if ((bus = device_add_child(parent, "pci", DEVICE_UNIT_ANY)) == NULL)
		return (NULL);
	if (device_set_driver(bus, &xg_pci_bus_driver) != 0) {
		device_delete_child(parent, bus);
		return (NULL);
	}
	sc = device_get_softc(bus);
	sc->domain = domain;
	sc->busno = busno;
	bus->state = DS_ALIVE;
	if (device_attach(bus) != 0) {
		device_delete_child(parent, bus);
		return (NULL);
	}
	return (bus);
}

static device_t
xg_pci_find_bus(u_int domain, u_int busno)
{
	devclass_t dc = devclass_find("pci");
	struct xg_pci_softc *sc;

	for (int i = 0; dc != NULL && i < dc->maxunit; i++) {
		if (dc->devices[i] == NULL || dc->devices[i]->driver != &xg_pci_bus_driver)
			continue;
		sc = device_get_softc(dc->devices[i]);
		if (sc->domain == domain && sc->busno == busno)
			return (dc->devices[i]);
	}
	return (NULL);
}

/* the config header and capability list, read only (pci_read_device + pci_read_cap) */
static void
xg_pci_read_header(struct xg_pci_dinfo *di)
{
	pcicfgregs *cfg = &di->pd.cfg;
	xg_pci_xnu_t *h = di->xh;
	int nextptr, guard;
	uint8_t ptr;
	uint32_t val;

#define REG(r, w)	xg_pci_xnu_cfg_read(h, (r), (w))
	cfg->vendor = (uint16_t)REG(PCIR_VENDOR, 2);
	cfg->device = (uint16_t)REG(PCIR_DEVICE, 2);
	cfg->cmdreg = (uint16_t)REG(PCIR_COMMAND, 2);
	cfg->statreg = (uint16_t)REG(PCIR_STATUS, 2);
	cfg->baseclass = (uint8_t)REG(PCIR_CLASS, 1);
	cfg->subclass = (uint8_t)REG(PCIR_SUBCLASS, 1);
	cfg->progif = (uint8_t)REG(PCIR_PROGIF, 1);
	cfg->revid = (uint8_t)REG(PCIR_REVID, 1);
	cfg->hdrtype = (uint8_t)REG(PCIR_HDRTYPE, 1) & PCIM_HDRTYPE;
	cfg->mfdev = ((uint8_t)REG(PCIR_HDRTYPE, 1) & PCIM_MFDEV) != 0;
	cfg->cachelnsz = (uint8_t)REG(PCIR_CACHELNSZ, 1);
	cfg->intpin = (uint8_t)REG(PCIR_INTPIN, 1);
	cfg->intline = (uint8_t)REG(PCIR_INTLINE, 1);
	cfg->lattimer = (uint8_t)REG(PCIR_LATTIMER, 1);
	switch (cfg->hdrtype) {
	case PCIM_HDRTYPE_NORMAL:
		cfg->subvendor = (uint16_t)REG(PCIR_SUBVEND_0, 2);
		cfg->subdevice = (uint16_t)REG(PCIR_SUBDEV_0, 2);
		cfg->mingnt = (uint8_t)REG(PCIR_MINGNT, 1);
		cfg->maxlat = (uint8_t)REG(PCIR_MAXLAT, 1);
		cfg->nummaps = PCI_MAXMAPS_0;
		break;
	case PCIM_HDRTYPE_BRIDGE:
		cfg->bridge.br_seclat = (uint8_t)REG(PCIR_SECLAT_1, 1);
		cfg->bridge.br_subbus = (uint8_t)REG(PCIR_SUBBUS_1, 1);
		cfg->bridge.br_secbus = (uint8_t)REG(PCIR_SECBUS_1, 1);
		cfg->bridge.br_pribus = (uint8_t)REG(PCIR_PRIBUS_1, 1);
		cfg->bridge.br_control = (uint16_t)REG(PCIR_BRIDGECTL_1, 2);
		cfg->nummaps = PCI_MAXMAPS_1;
		break;
	default:
		cfg->nummaps = 0;
		break;
	}
	if (!(cfg->statreg & PCIM_STATUS_CAPPRESENT))
		return;
	nextptr = (uint8_t)REG(cfg->hdrtype == PCIM_HDRTYPE_CARDBUS ? PCIR_CAP_PTR_2 : PCIR_CAP_PTR, 1);
	/* a broken list could loop; 48 is every possible slot in the legacy space */
	for (guard = 0; nextptr != 0 && guard < 48; guard++) {
		if (nextptr < 0x40 || nextptr > 0xfc)
			break;
		ptr = (uint8_t)nextptr;
		nextptr = (uint8_t)REG(ptr + PCICAP_NEXTPTR, 1);
		switch ((uint8_t)REG(ptr + PCICAP_ID, 1)) {
		case PCIY_PMG:
			cfg->pp.pp_location = ptr;
			cfg->pp.pp_cap = (uint16_t)REG(ptr + PCIR_POWER_CAP, 2);
			break;
		case PCIY_MSI:
			cfg->msi.msi_location = ptr;
			cfg->msi.msi_ctrl = (uint16_t)REG(ptr + PCIR_MSI_CTRL, 2);
			break;
		case PCIY_MSIX:
			cfg->msix.msix_location = ptr;
			cfg->msix.msix_ctrl = (uint16_t)REG(ptr + PCIR_MSIX_CTRL, 2);
			val = REG(ptr + PCIR_MSIX_TABLE, 4);
			cfg->msix.msix_table_bar = PCIR_BAR(val & PCIM_MSIX_BIR_MASK);
			cfg->msix.msix_table_offset = val & ~PCIM_MSIX_BIR_MASK;
			val = REG(ptr + PCIR_MSIX_PBA, 4);
			cfg->msix.msix_pba_bar = PCIR_BAR(val & PCIM_MSIX_BIR_MASK);
			cfg->msix.msix_pba_offset = val & ~PCIM_MSIX_BIR_MASK;
			break;
		case PCIY_VPD:
			cfg->vpd.vpd_reg = ptr;
			break;
		case PCIY_SUBVENDOR:
			if (cfg->hdrtype == PCIM_HDRTYPE_BRIDGE) {
				val = REG(ptr + PCIR_SUBVENDCAP_ID, 4);
				cfg->subvendor = val & 0xffff;
				cfg->subdevice = val >> 16;
			}
			break;
		case PCIY_PCIX:
			cfg->pcix.pcix_location = ptr;
			break;
		case PCIY_EXPRESS:
			cfg->pcie.pcie_location = ptr;
			cfg->pcie.pcie_flags = (uint16_t)REG(ptr + PCIER_FLAGS, 2);
			cfg->pcie.pcie_type = cfg->pcie.pcie_flags & PCIEM_FLAGS_TYPE;
			break;
		}
	}
#undef REG
}

/* BARs from IOKit's assigned ranges; the raw BAR value is only read to learn its type */
static void
xg_pci_read_bars(struct xg_pci_dinfo *di)
{
	pcicfgregs *cfg = &di->pd.cfg;
	unsigned long long base, len;
	struct pci_map *pm;
	uint64_t value;
	int reg, last;

	STAILQ_INIT(&cfg->maps);
	last = PCIR_BAR(cfg->nummaps);
	for (reg = PCIR_BAR(0); reg <= last; reg += 4) {
		if (reg == last) {
			if (cfg->hdrtype != PCIM_HDRTYPE_NORMAL)
				break;
			reg = PCIR_BIOS;
		}
		if (xg_pci_xnu_bar(di->xh, reg, &base, &len) != 0 || len == 0)
			continue;
		value = xg_pci_xnu_cfg_read(di->xh, reg, 4);
		if (reg != PCIR_BIOS && PCI_BAR_MEM(value) && (value & PCIM_BAR_MEM_TYPE) == PCIM_BAR_MEM_64)
			value |= (uint64_t)xg_pci_xnu_cfg_read(di->xh, reg + 4, 4) << 32;
		pm = malloc(sizeof(*pm), M_XGBUS, M_WAITOK | M_ZERO);
		pm->pm_reg = (uint16_t)reg;
		pm->pm_value = value;
		pm->pm_size = flsll(len) - 1;	/* log2, as pci_mapsize() returns */
		STAILQ_INSERT_TAIL(&cfg->maps, pm, pm_link);
		if (reg != PCIR_BIOS && PCI_BAR_MEM(value) && (value & PCIM_BAR_MEM_TYPE) == PCIM_BAR_MEM_64)
			reg += 4;
		if (reg == PCIR_BIOS)
			break;
	}
}

static struct xg_pci_dinfo *
xg_pci_find_xh(xg_pci_xnu_t *h)
{
	struct pci_devinfo *pd;

	STAILQ_FOREACH(pd, &pci_devq, pci_links)
		if (((struct xg_pci_dinfo *)pd)->xh == h)
			return ((struct xg_pci_dinfo *)pd);
	return (NULL);
}

/* one IOPCIDevice into the tree as a driverless child of its bus, as pci_add_child does */
int
xg_pci_bus_add(xg_pci_xnu_t *h)
{
	unsigned int domain, busno, slot, func;
	struct xg_pci_dinfo *di;
	pcicfgregs *cfg;
	device_t bus, child;
	int idx, irq, error = 0;

	xg_pci_once();
	if (xg_pci_xnu_cfg_read(h, PCIR_VENDOR, 2) == 0xffff)
		return (ENXIO);
	bus_topo_lock();
	if (xg_pci_find_xh(h) != NULL)
		goto out;
	xg_pci_xnu_location(h, &domain, &busno, &slot, &func);
	if ((bus = xg_pci_find_bus(domain, busno)) == NULL &&
	    (bus = xg_pci_new_bus(xg_pci_hostb, domain, busno)) == NULL) {
		error = ENOMEM;
		goto out;
	}
	di = malloc(sizeof(*di), M_DEVBUF, M_WAITOK | M_ZERO);
	di->xh = h;
	cfg = &di->pd.cfg;
	cfg->domain = domain;
	cfg->bus = (uint8_t)busno;
	cfg->slot = (uint8_t)slot;
	cfg->func = (uint8_t)func;
	xg_pci_read_header(di);
	xg_pci_read_bars(di);
	STAILQ_INIT(&di->pd.resources);
	di->pd.conf.pc_sel.pc_domain = domain;
	di->pd.conf.pc_sel.pc_bus = (uint8_t)busno;
	di->pd.conf.pc_sel.pc_dev = (uint8_t)slot;
	di->pd.conf.pc_sel.pc_func = (uint8_t)func;
	di->pd.conf.pc_hdr = cfg->hdrtype;
	di->pd.conf.pc_subvendor = cfg->subvendor;
	di->pd.conf.pc_subdevice = cfg->subdevice;
	di->pd.conf.pc_vendor = cfg->vendor;
	di->pd.conf.pc_device = cfg->device;
	di->pd.conf.pc_class = cfg->baseclass;
	di->pd.conf.pc_subclass = cfg->subclass;
	di->pd.conf.pc_progif = cfg->progif;
	di->pd.conf.pc_revid = cfg->revid;
	/* rid 0 is the legacy INTx line, when IOKit has one for it */
	if (cfg->intpin != 0 && (idx = xg_pci_xnu_intx_index(h)) >= 0 &&
	    (irq = xg_pci_irq_get(h, idx, cfg->intline, false)) >= 0)
		xg_pci_rle_add(&di->pd.resources, SYS_RES_IRQ, 0, irq, irq, 1);
	if ((child = device_add_child(bus, NULL, DEVICE_UNIT_ANY)) == NULL) {
		free(di, M_DEVBUF);
		error = ENOMEM;
		goto out;
	}
	cfg->dev = child;
	device_set_ivars(child, di);
	STAILQ_INSERT_TAIL(&pci_devq, &di->pd, pci_links);
	/* bridges get pcib (and the bus behind them), host bridges hostb, as FreeBSD's drivers would claim */
	if (cfg->hdrtype == PCIM_HDRTYPE_BRIDGE)
		error = xg_pci_force_driver(child, "pcib", &xg_pcib_driver);
	else if (cfg->baseclass == PCIC_BRIDGE && cfg->subclass == PCIS_BRIDGE_HOST)
		error = xg_pci_force_driver(child, "hostb", &xg_hostb_driver);
out:
	bus_topo_unlock();
	return (error);
}

/* the device IOKit handed us: vgapci on it, drmn under that, then whatever LinuxKPI driver matches */
int
xg_pci_bus_claim(xg_pci_xnu_t *h)
{
	struct xg_pci_dinfo *di;
	device_t dev;
	int error;

	bus_topo_lock();
	if ((di = xg_pci_find_xh(h)) == NULL) {
		error = ENOENT;
		goto out;
	}
	dev = di->pd.cfg.dev;
	if (dev->driver == &xg_vgapci_driver) {
		error = 0;
		goto out;
	}
	if (di->pd.cfg.baseclass != PCIC_DISPLAY) {
		error = ENXIO;
		goto out;
	}
	if (dev->state >= DS_ATTACHED) {
		error = EBUSY;
		goto out;
	}
	error = xg_pci_force_driver(dev, "vgapci", &xg_vgapci_driver);
out:
	bus_topo_unlock();
	return (error);
}

int
xg_pci_bus_unclaim(xg_pci_xnu_t *h)
{
	struct xg_pci_dinfo *di;
	int error = 0;

	bus_topo_lock();
	if ((di = xg_pci_find_xh(h)) != NULL && di->pd.cfg.dev->driver == &xg_vgapci_driver)
		error = device_detach(di->pd.cfg.dev);
	bus_topo_unlock();
	return (error);
}

/*
 * Lookups over every function IOKit showed us (pci.c)
 */

bool
is_pci_device(device_t dev)
{
	devclass_t pci_class;

	if (device_get_parent(dev) == NULL)
		return (false);
	pci_class = devclass_find("pci");
	return (device_get_devclass(device_get_parent(dev)) == pci_class);
}

device_t
pci_find_dbsf(uint32_t domain, uint8_t bus, uint8_t slot, uint8_t func)
{
	struct pci_devinfo *dinfo;

	STAILQ_FOREACH(dinfo, &pci_devq, pci_links)
		if (dinfo->cfg.domain == domain && dinfo->cfg.bus == bus && dinfo->cfg.slot == slot &&
		    dinfo->cfg.func == func)
			return (dinfo->cfg.dev);
	return (NULL);
}

device_t
pci_find_bsf(uint8_t bus, uint8_t slot, uint8_t func)
{
	return (pci_find_dbsf(0, bus, slot, func));
}

device_t
pci_find_device(uint16_t vendor, uint16_t device)
{
	struct pci_devinfo *dinfo;

	STAILQ_FOREACH(dinfo, &pci_devq, pci_links)
		if (dinfo->cfg.vendor == vendor && dinfo->cfg.device == device)
			return (dinfo->cfg.dev);
	return (NULL);
}

device_t
pci_find_class(uint8_t class, uint8_t subclass)
{
	return (pci_find_class_from(class, subclass, NULL));
}

device_t
pci_find_class_from(uint8_t class, uint8_t subclass, device_t from)
{
	struct pci_devinfo *dinfo;
	bool found = false;

	STAILQ_FOREACH(dinfo, &pci_devq, pci_links) {
		if (from != NULL && !found) {
			if (from == dinfo->cfg.dev)
				found = true;
			continue;
		}
		if (dinfo->cfg.baseclass == class && dinfo->cfg.subclass == subclass)
			return (dinfo->cfg.dev);
	}
	return (NULL);
}

device_t
pci_find_base_class_from(uint8_t class, device_t from)
{
	struct pci_devinfo *dinfo;
	bool found = false;

	STAILQ_FOREACH(dinfo, &pci_devq, pci_links) {
		if (from != NULL && !found) {
			if (from == dinfo->cfg.dev)
				found = true;
			continue;
		}
		if (dinfo->cfg.baseclass == class)
			return (dinfo->cfg.dev);
	}
	return (NULL);
}

device_t
pci_find_pcie_root_port(device_t dev)
{
	struct pci_devinfo *dinfo;
	device_t pcib, bus;

	KASSERT(is_pci_device(dev), ("%s: non-pci device %s", __func__, device_get_nameunit(dev)));
	for (;;) {
		if ((bus = device_get_parent(dev)) == NULL || (pcib = device_get_parent(bus)) == NULL)
			return (NULL);
		if (!is_pci_device(pcib))
			return (NULL);
		dinfo = device_get_ivars(pcib);
		if (dinfo->cfg.pcie.pcie_location != 0 && dinfo->cfg.pcie.pcie_type == PCIEM_TYPE_ROOT_PORT)
			return (pcib);
		dev = pcib;
	}
}

/* pci_cfg_save(dev, dinfo, 0): refresh the cached header so a later restore has current values */
void
pci_save_state(device_t dev)
{
	struct pci_devinfo *dinfo = device_get_ivars(dev);
	pcicfgregs *cfg = &dinfo->cfg;
	int loc = cfg->pcie.pcie_location;

	cfg->vendor = (uint16_t)pci_read_config(dev, PCIR_VENDOR, 2);
	cfg->device = (uint16_t)pci_read_config(dev, PCIR_DEVICE, 2);
	cfg->cmdreg = (uint16_t)pci_read_config(dev, PCIR_COMMAND, 2);
	cfg->intline = (uint8_t)pci_read_config(dev, PCIR_INTLINE, 1);
	cfg->intpin = (uint8_t)pci_read_config(dev, PCIR_INTPIN, 1);
	cfg->cachelnsz = (uint8_t)pci_read_config(dev, PCIR_CACHELNSZ, 1);
	cfg->lattimer = (uint8_t)pci_read_config(dev, PCIR_LATTIMER, 1);
	cfg->baseclass = (uint8_t)pci_read_config(dev, PCIR_CLASS, 1);
	cfg->subclass = (uint8_t)pci_read_config(dev, PCIR_SUBCLASS, 1);
	cfg->progif = (uint8_t)pci_read_config(dev, PCIR_PROGIF, 1);
	cfg->revid = (uint8_t)pci_read_config(dev, PCIR_REVID, 1);
	if (cfg->hdrtype == PCIM_HDRTYPE_NORMAL) {
		cfg->subvendor = (uint16_t)pci_read_config(dev, PCIR_SUBVEND_0, 2);
		cfg->subdevice = (uint16_t)pci_read_config(dev, PCIR_SUBDEV_0, 2);
		cfg->mingnt = (uint8_t)pci_read_config(dev, PCIR_MINGNT, 1);
		cfg->maxlat = (uint8_t)pci_read_config(dev, PCIR_MAXLAT, 1);
	} else if (cfg->hdrtype == PCIM_HDRTYPE_BRIDGE) {
		cfg->bridge.br_seclat = (uint8_t)pci_read_config(dev, PCIR_SECLAT_1, 1);
		cfg->bridge.br_subbus = (uint8_t)pci_read_config(dev, PCIR_SUBBUS_1, 1);
		cfg->bridge.br_secbus = (uint8_t)pci_read_config(dev, PCIR_SECBUS_1, 1);
		cfg->bridge.br_pribus = (uint8_t)pci_read_config(dev, PCIR_PRIBUS_1, 1);
		cfg->bridge.br_control = (uint16_t)pci_read_config(dev, PCIR_BRIDGECTL_1, 2);
	}
	if (loc != 0) {
		cfg->pcie.pcie_flags = (uint16_t)pci_read_config(dev, loc + PCIER_FLAGS, 2);
		cfg->pcie.pcie_device_ctl = (uint16_t)pci_read_config(dev, loc + PCIER_DEVICE_CTL, 2);
		cfg->pcie.pcie_link_ctl = (uint16_t)pci_read_config(dev, loc + PCIER_LINK_CTL, 2);
		cfg->pcie.pcie_slot_ctl = (uint16_t)pci_read_config(dev, loc + PCIER_SLOT_CTL, 2);
		cfg->pcie.pcie_root_ctl = (uint16_t)pci_read_config(dev, loc + PCIER_ROOT_CTL, 2);
		cfg->pcie.pcie_device_ctl2 = (uint16_t)pci_read_config(dev, loc + PCIER_DEVICE_CTL2, 2);
		cfg->pcie.pcie_link_ctl2 = (uint16_t)pci_read_config(dev, loc + PCIER_LINK_CTL2, 2);
		cfg->pcie.pcie_slot_ctl2 = (uint16_t)pci_read_config(dev, loc + PCIER_SLOT_CTL2, 2);
	}
}

/*
 * Bring-up. Lazy, so the first newbus call from any SYSINIT works, but xg_pci_init() should run before
 * linux_compat_init() reads root_bus.
 */
static void
xg_pci_once(void)
{
	if (atomic_load_acq_int(&xg_pci_inited) == 2)
		return;
	if (!atomic_cmpset_int(&xg_pci_inited, 0, 1)) {
		while (atomic_load_acq_int(&xg_pci_inited) != 2)
			cpu_spinwait();
		return;
	}
	mtx_init(&xg_pci_kobj_mtx, "kobj", NULL, MTX_DEF);
	mtx_init(&xg_pci_topo_mtx, "bus topo", NULL, MTX_DEF | MTX_RECURSE);
	mtx_init(&xg_pci_irq_mtx, "xg pci irq", NULL, MTX_DEF);
	atomic_store_rel_int(&xg_pci_inited, 2);

	if (bus_dma_tag_create(NULL, 1, (bus_addr_t)1 << 32, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL, NULL,
	    BUS_SPACE_MAXSIZE, BUS_SPACE_UNRESTRICTED, BUS_SPACE_MAXSIZE, 0, NULL, NULL, &xg_pci_dmat) != 0)
		panic("xegfx: no PCI DMA tag");
	bus_topo_lock();
	xg_pci_root = xg_pci_make_device(NULL, "root", 0);
	if (xg_pci_root == NULL || device_set_driver(xg_pci_root, &xg_root_driver) != 0)
		panic("xegfx: no root0");
	xg_pci_root->state = DS_ATTACHED;
	xg_pci_hostb = device_add_child(xg_pci_root, "pcib", DEVICE_UNIT_ANY);
	if (xg_pci_hostb == NULL || xg_pci_force_driver(xg_pci_hostb, "pcib", &xg_pcib_driver) != 0)
		panic("xegfx: no pcib0");
	root_bus = xg_pci_root;
	bus_topo_unlock();
}

void
xg_pci_init(void)
{
	xg_pci_once();
}

/* the newbus tree into the console ring, so a boot shows which driver took (or did not take) each device */
static void
xg_newbus_dump_one(device_t dev, int depth)
{
	device_t c;

	if (dev->parent != NULL && dev->parent->devclass != NULL &&
	    strcmp(devclass_get_name(dev->parent->devclass), "pci") == 0 && dev->ivars != NULL)
		printf("xegfx: newbus %*s%s state %d driver %s pci %04x:%04x class %06x\n", depth * 2, "",
		    dev->nameunit ? dev->nameunit : "?", dev->state, dev->driver ? dev->driver->name : "-",
		    pci_get_vendor(dev), pci_get_device(dev),
		    (pci_get_class(dev) << 16) | (pci_get_subclass(dev) << 8) | pci_get_progif(dev));
	else
		printf("xegfx: newbus %*s%s state %d driver %s\n", depth * 2, "",
		    dev->nameunit ? dev->nameunit : "?", dev->state, dev->driver ? dev->driver->name : "-");
	TAILQ_FOREACH(c, &dev->children, link)
		xg_newbus_dump_one(c, depth + 1);
}

void
xg_newbus_dump(void)
{
	devclass_t dc;

	if (xg_pci_root != NULL)
		xg_newbus_dump_one(xg_pci_root, 0);
	TAILQ_FOREACH(dc, &xg_pci_devclasses, link) {
		struct driverlink *dl;

		printf("xegfx: devclass %s:", devclass_get_name(dc));
		TAILQ_FOREACH(dl, &dc->drivers, link)
			printf(" %s", dl->driver->name);
		printf("\n");
	}
}

/* 1 when a drm driver (i915kms or xe) is attached to some device; stage 4 fails without one */
int
xg_drm_attached(void)
{
	device_t d;

	TAILQ_FOREACH(d, &xg_pci_devices, devlink)
		if (d->state == DS_ATTACHED && d->driver != NULL &&
		    (strncmp(d->driver->name, "i915", 4) == 0 || strncmp(d->driver->name, "xe", 2) == 0))
			return (1);
	return (0);
}
