/*
 * XeGfx: the narrow interface between the FreeBSD newbus/PCI emulation (shim/src/fbsd/xg_pci.c) and its IOKit side
 * (shim/src/xnu/xg_pci.cpp). Plain C types only, so both a FreeBSD-side .c file and an XNU-side .cpp can include it.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#ifndef _XG_PCI_XNU_H_
#define _XG_PCI_XNU_H_

/* one IOPCIDevice, retained by the IOKit side for the life of the kext */
typedef struct xg_pci_xnu xg_pci_xnu_t;

/* FreeBSD filter return bits, same values as sys/bus.h */
#define XG_PCI_FILTER_STRAY	0x01
#define XG_PCI_FILTER_HANDLED	0x02
#define XG_PCI_FILTER_SCHEDULE	0x04

#ifdef __cplusplus
extern "C" {
#endif

/* IOKit side, called by xg_pci.c */
unsigned int xg_pci_xnu_cfg_read(xg_pci_xnu_t *h, int reg, int width);
void xg_pci_xnu_cfg_write(xg_pci_xnu_t *h, int reg, unsigned int val, int width);
void xg_pci_xnu_location(xg_pci_xnu_t *h, unsigned int *domain, unsigned int *bus, unsigned int *slot,
    unsigned int *func);
int xg_pci_xnu_bar(xg_pci_xnu_t *h, int reg, unsigned long long *base, unsigned long long *len);
int xg_pci_xnu_map(xg_pci_xnu_t *h, int reg, unsigned long *va, void **map);
void xg_pci_xnu_unmap(void *map);
int xg_pci_xnu_intx_index(xg_pci_xnu_t *h);
int xg_pci_xnu_msi_alloc(xg_pci_xnu_t *h, int msix, int want, int *index, int max);
int xg_pci_xnu_intr_setup(xg_pci_xnu_t *h, int index, int (*filter)(void *), void (*handler)(void *), void *arg,
    void **cookie);
void xg_pci_xnu_intr_teardown(void *cookie);
void xg_pci_xnu_intr_drain(void *cookie);

/* FreeBSD side, called by xg_pci.cpp */
void xg_pci_init(void);
int xg_pci_bus_add(xg_pci_xnu_t *h);
int xg_pci_bus_claim(xg_pci_xnu_t *h);
int xg_pci_bus_unclaim(xg_pci_xnu_t *h);

/*
 * Entry points for the kext's IOService (xg_kext.cpp). start enumerates every IOPCIDevice once into the newbus
 * tree, then attaches vgapci + drmn to this one so a registered LinuxKPI pci_driver probes it. Both return an errno.
 */
int xg_pci_xnu_start(void *iopcidevice);
int xg_pci_xnu_stop(void *iopcidevice);

#ifdef __cplusplus
}
#endif
#endif
