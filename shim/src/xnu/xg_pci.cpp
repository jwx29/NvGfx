/*
 * XeGfx: the IOKit side of the PCI emulation. Wraps every IOPCIDevice for xg_pci.c (config space, BAR ranges and
 * mappings, interrupt sources) and hands the kext's own IOPCIDevice to newbus so LinuxKPI can probe it.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/OSAtomic.h>
#include <sys/errno.h>
#include "xg_pci_xnu.h"

/* IOPCIDevice is deprecated for DriverKit, but it is the only PCI provider a kext gets */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

struct xg_pci_xnu {
	IOPCIDevice	*pci;
	xg_pci_xnu	*next;
};

static xg_pci_xnu *gXgPciDevs;
static IOLock *gXgPciLock;
static bool gXgPciScanned;
extern "C" { unsigned int xg_pci_irqs; }	/* every interrupt taken; stage 5 reports how many its requests raised */

/* the owner of one interrupt source: it carries the FreeBSD filter/handler pair */
class XgPciIntr : public OSObject {
	OSDeclareDefaultStructors(XgPciIntr)
public:
	IOWorkLoop		*wl;
	IOInterruptEventSource	*src;
	int			(*filter)(void *);
	void			(*handler)(void *);
	void			*arg;

	bool
	init() override
	{
		return (OSObject::init());
	}
};
OSDefineMetaClassAndStructors(XgPciIntr, OSObject)

extern "C" unsigned int
xg_pci_xnu_cfg_read(xg_pci_xnu_t *h, int reg, int width)
{
	switch (width) {
	case 1:
		return (h->pci->extendedConfigRead8((IOByteCount)reg));
	case 2:
		return (h->pci->extendedConfigRead16((IOByteCount)reg));
	case 4:
		return (h->pci->extendedConfigRead32((IOByteCount)reg));
	}
	return (~0u);
}

extern "C" void
xg_pci_xnu_cfg_write(xg_pci_xnu_t *h, int reg, unsigned int val, int width)
{
	switch (width) {
	case 1:
		h->pci->extendedConfigWrite8((IOByteCount)reg, (UInt8)val);
		break;
	case 2:
		h->pci->extendedConfigWrite16((IOByteCount)reg, (UInt16)val);
		break;
	case 4:
		h->pci->extendedConfigWrite32((IOByteCount)reg, val);
		break;
	}
}

/* IOPCIDevice has no segment accessor in the SDK; every Mac and PC we target has one segment */
extern "C" void
xg_pci_xnu_location(xg_pci_xnu_t *h, unsigned int *domain, unsigned int *bus, unsigned int *slot,
    unsigned int *func)
{
	*domain = 0;
	*bus = h->pci->getBusNumber();
	*slot = h->pci->getDeviceNumber();
	*func = h->pci->getFunctionNumber();
}

extern "C" int
xg_pci_xnu_bar(xg_pci_xnu_t *h, int reg, unsigned long long *base, unsigned long long *len)
{
	IODeviceMemory *m = h->pci->getDeviceMemoryWithRegister((UInt8)reg);

	if (m == NULL)
		return (ENOENT);
	*base = m->getPhysicalAddress();
	*len = m->getLength();
	return (0);
}

extern "C" int
xg_pci_xnu_map(xg_pci_xnu_t *h, int reg, unsigned long *va, void **map)
{
	IOMemoryMap *m = h->pci->mapDeviceMemoryWithRegister((UInt8)reg, kIOMapInhibitCache);

	if (m == NULL)
		return (ENOMEM);
	*va = (unsigned long)m->getVirtualAddress();
	*map = m;
	return (0);
}

extern "C" void
xg_pci_xnu_unmap(void *map)
{
	if (map != NULL)
		((IOMemoryMap *)map)->release();
}

/* the first non-message interrupt index: the INTx line */
extern "C" int
xg_pci_xnu_intx_index(xg_pci_xnu_t *h)
{
	int type;

	for (int i = 0; i < 64; i++) {
		if (h->pci->getInterruptType(i, &type) != kIOReturnSuccess)
			break;
		if (!(type & kIOInterruptTypePCIMessaged))
			return (i);
	}
	return (-1);
}

static int
xg_pci_msg_indexes(IOPCIDevice *pci, int *index, int max)
{
	int n = 0, type;

	for (int i = 0; i < 256 && n < max; i++) {
		if (pci->getInterruptType(i, &type) != kIOReturnSuccess)
			break;
		if (type & kIOInterruptTypePCIMessaged)
			index[n++] = i;
	}
	return (n);
}

/* message interrupt indexes, asking IOPCIFamily for vectors only when it has not set enough up already */
extern "C" int
xg_pci_xnu_msi_alloc(xg_pci_xnu_t *h, int msix, int want, int *index, int max)
{
	int n;

	if (want > max)
		want = max;
	n = xg_pci_msg_indexes(h->pci, index, want);
	if (n >= want)
		return (n);
	if (h->pci->configureInterrupts(msix ? kIOInterruptTypePCIMessagedX : kIOInterruptTypePCIMessaged, 1,
	    (UInt32)want) != kIOReturnSuccess && n == 0)
		return (-ENXIO);
	n = xg_pci_msg_indexes(h->pci, index, want);
	return (n > 0 ? n : -ENXIO);
}

static bool
xg_pci_filter(OSObject *owner, IOFilterInterruptEventSource *src)
{
	XgPciIntr *o = (XgPciIntr *)owner;
	int r;

	OSIncrementAtomic((volatile SInt32 *)&xg_pci_irqs);
	r = o->filter(o->arg);

	return ((r & XG_PCI_FILTER_SCHEDULE) != 0 && o->handler != NULL);
}

static void
xg_pci_action(OSObject *owner, IOInterruptEventSource *src, int count)
{
	XgPciIntr *o = (XgPciIntr *)owner;

	if (o->filter == NULL)
		OSIncrementAtomic((volatile SInt32 *)&xg_pci_irqs);
	if (o->handler != NULL)
		o->handler(o->arg);
}

static IOReturn
xg_pci_noop(OSObject *, void *, void *, void *, void *)
{
	return (kIOReturnSuccess);
}

extern "C" int
xg_pci_xnu_intr_setup(xg_pci_xnu_t *h, int index, int (*filter)(void *), void (*handler)(void *), void *arg,
    void **cookie)
{
	XgPciIntr *o = OSTypeAlloc(XgPciIntr);

	if (o == NULL || !o->init()) {
		if (o != NULL)
			o->release();
		return (ENOMEM);
	}
	o->filter = filter;
	o->handler = handler;
	o->arg = arg;
	/* one work loop per vector, so one slow handler does not hold up the others */
	if ((o->wl = IOWorkLoop::workLoop()) == NULL) {
		o->release();
		return (ENOMEM);
	}
	if (filter != NULL)
		o->src = IOFilterInterruptEventSource::filterInterruptEventSource(o, xg_pci_action, xg_pci_filter,
		    h->pci, index);
	else
		o->src = IOInterruptEventSource::interruptEventSource(o, xg_pci_action, h->pci, index);
	if (o->src == NULL || o->wl->addEventSource(o->src) != kIOReturnSuccess) {
		if (o->src != NULL)
			o->src->release();
		o->wl->release();
		o->release();
		return (ENXIO);
	}
	o->src->enable();
	*cookie = o;
	return (0);
}

extern "C" void
xg_pci_xnu_intr_teardown(void *cookie)
{
	XgPciIntr *o = (XgPciIntr *)cookie;

	o->src->disable();
	/* takes the work loop gate, so a running handler finishes first */
	o->wl->removeEventSource(o->src);
	o->src->release();
	o->wl->release();
	o->release();
}

extern "C" void
xg_pci_xnu_intr_drain(void *cookie)
{
	XgPciIntr *o = (XgPciIntr *)cookie;

	o->wl->runAction(xg_pci_noop, o);
}

static xg_pci_xnu *
xg_pci_lookup(IOPCIDevice *pci)
{
	for (xg_pci_xnu *h = gXgPciDevs; h != NULL; h = h->next)
		if (h->pci == pci)
			return (h);
	return (NULL);
}

static unsigned int
xg_pci_key(IOPCIDevice *pci)
{
	return ((unsigned int)pci->getBusNumber() << 8 | (unsigned int)pci->getDeviceNumber() << 3 |
	    pci->getFunctionNumber());
}

/*
 * Only what i915 needs on bus 0: the display function, the host bridge (MCHBAR) and the ISA/LPC bridge (PCH type).
 * N-216 froze seconds after a scan of every device; resolving interrupts on devices other drivers own is the
 * suspect. xegfx.pci_scan_all=1 (debug.xegfx_env or boot-args) brings the full scan back for experiments.
 */
extern "C" int getenv_int(const char *name, int *data);

static bool
xg_pci_wanted(IOPCIDevice *pci)
{
	int all = 0;
	uint8_t cls = pci->configRead8(kIOPCIConfigClassCode + 2), sub = pci->configRead8(kIOPCIConfigClassCode + 1);

	if (getenv_int("xegfx.pci_scan_all", &all) && all)
		return true;
	if (pci->getBusNumber() != 0)
		return false;
	return cls == 0x03 || (cls == 0x06 && (sub == 0x00 || sub == 0x01));
}

/*
 * Every wanted IOPCIDevice, in bus order: a bridge always sits on a lower bus number than the bus behind it, so newbus
 * sees each bridge before the devices it leads to. Config reads only.
 */
static int
xg_pci_scan(void)
{
	OSDictionary *match;
	OSIterator *it;
	OSObject *obj;
	xg_pci_xnu **v = NULL, *h;
	size_t n = 0, cap = 0;

	if (gXgPciScanned)
		return (0);
	if ((match = IOService::serviceMatching("IOPCIDevice")) == NULL)
		return (ENOMEM);
	it = IOService::getMatchingServices(match);
	match->release();
	if (it == NULL)
		return (ENOENT);
	while ((obj = it->getNextObject()) != NULL) {
		IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, obj);

		if (pci == NULL || xg_pci_lookup(pci) != NULL || !xg_pci_wanted(pci))
			continue;
		if ((h = (xg_pci_xnu *)IOMallocZero(sizeof(*h))) == NULL)
			break;
		pci->retain();
		h->pci = pci;
		h->next = gXgPciDevs;
		gXgPciDevs = h;
		if (n == cap) {
			size_t ncap = cap ? cap * 2 : 64;
			xg_pci_xnu **nv = (xg_pci_xnu **)IOMallocZero(ncap * sizeof(*nv));

			if (nv == NULL)
				break;
			if (v != NULL) {
				memcpy(nv, v, n * sizeof(*nv));
				IOFree(v, cap * sizeof(*v));
			}
			v = nv;
			cap = ncap;
		}
		v[n++] = h;
	}
	it->release();
	/* insertion sort: a few dozen functions */
	for (size_t i = 1; i < n; i++) {
		xg_pci_xnu *t = v[i];
		size_t j = i;

		for (; j > 0 && xg_pci_key(v[j - 1]->pci) > xg_pci_key(t->pci); j--)
			v[j] = v[j - 1];
		v[j] = t;
	}
	for (size_t i = 0; i < n; i++)
		xg_pci_bus_add(v[i]);
	if (v != NULL)
		IOFree(v, cap * sizeof(*v));
	gXgPciScanned = true;
	return (0);
}

extern "C" int
xg_pci_xnu_start(void *iopcidevice)
{
	IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, (OSObject *)iopcidevice);
	xg_pci_xnu *h;
	int error;

	if (pci == NULL)
		return (EINVAL);
	if (gXgPciLock == NULL) {
		IOLock *l = IOLockAlloc();

		if (l == NULL)
			return (ENOMEM);
		if (!OSCompareAndSwapPtr(NULL, l, &gXgPciLock))
			IOLockFree(l);
	}
	xg_pci_init();
	IOLockLock(gXgPciLock);
	error = xg_pci_scan();
	h = xg_pci_lookup(pci);
	IOLockUnlock(gXgPciLock);
	if (error != 0)
		return (error);
	if (h == NULL)
		return (ENOENT);
	return (xg_pci_bus_claim(h));
}

extern "C" int
xg_pci_xnu_stop(void *iopcidevice)
{
	IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, (OSObject *)iopcidevice);
	xg_pci_xnu *h;

	if (pci == NULL || gXgPciLock == NULL)
		return (EINVAL);
	IOLockLock(gXgPciLock);
	h = xg_pci_lookup(pci);
	IOLockUnlock(gXgPciLock);
	return (h != NULL ? xg_pci_bus_unclaim(h) : ENOENT);
}
