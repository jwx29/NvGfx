/*
 * XeGfx dma area: the narrow C interface between xg_dma.c (FreeBSD side) and xg_dma.cpp (IOKit side), plus the
 * entry points other areas and the kext call. Plain C types only, so both sides can include it.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#ifndef _XG_DMA_H_
#define _XG_DMA_H_

/* xg_dma_xnu_iommu_state() results */
#define XG_DMA_IOMMU_NONE	0	/* no system mapper: bus address == physical address */
#define XG_DMA_IOMMU_UNKNOWN	1	/* platform has not decided yet */
#define XG_DMA_IOMMU_ACTIVE	2	/* AppleVTD (or another IOMapper) translates DMA */

/* xg_dma_acpi_notify() event kinds */
#define XG_DMA_ACPI_AC		0	/* AC adapter plugged or unplugged */
#define XG_DMA_ACPI_VIDEO	1	/* ACPI video notify (0x80-0x88) */

#ifdef __cplusplus
extern "C" {
#endif

/* IOKit side (xg_dma.cpp) */
int xg_dma_xnu_cfg_read32(unsigned int bus, unsigned int slot, unsigned int func, unsigned int off,
    unsigned int *val);
int xg_dma_xnu_iommu_state(void);

/* FreeBSD side (xg_dma.c) */
void xg_unimpl(const char *name) __attribute__((__noreturn__));	/* xg_kext.cpp */
void xg_dma_init(void);
int xg_dma_iommu_active(void);
void xg_dma_acpi_notify(int kind, unsigned int type, unsigned int data);
void xg_dma_acpi_set_target_state(unsigned int state);
int xg_dma_backlight_get(int unit, unsigned int *brightness);
int xg_dma_backlight_set(int unit, unsigned int brightness);

#ifdef __cplusplus
}
#endif
#endif
