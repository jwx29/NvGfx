/*
 * XeGfx dma area, IOKit side: PCI config reads of a device XeGfx does not own, and the system IOMMU state.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMapper.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <sys/errno.h>

#include "xg_dma.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"	/* IOPCIDevice: there is no in-kernel successor */

/*
 * Reads go through the IOPCIDevice that IOPCIFamily already published, never through CF8/CFC, so they take the
 * family's config lock. Nothing is matched or claimed: a later terminate would reset the iGPU (Legion I-093).
 */
extern "C" int
xg_dma_xnu_cfg_read32(unsigned int bus, unsigned int slot, unsigned int func, unsigned int off, unsigned int *val)
{
	OSDictionary *match;
	OSIterator *it;
	OSObject *obj;
	int error = ENOENT;

	if (off > 0xffc || (off & 3) != 0 || val == nullptr)
		return EINVAL;
	match = IOService::serviceMatching("IOPCIDevice");
	if (match == nullptr)
		return ENOMEM;
	it = IOService::getMatchingServices(match);
	match->release();
	if (it == nullptr)
		return ENOENT;
	while ((obj = it->getNextObject()) != nullptr) {
		IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, obj);

		if (pci == nullptr || pci->getBusNumber() != bus || pci->getDeviceNumber() != slot ||
		    pci->getFunctionNumber() != func)
			continue;
		*val = (off < 0x100) ? pci->configRead32((UInt8)off) : pci->extendedConfigRead32(off);
		error = 0;
		break;
	}
	it->release();
	return error;
}

/* IOMapper::gSystem is 0 (no mapper), 1 (unknown), 2 (one is coming) or a live AppleVTD mapper. */
extern "C" int
xg_dma_xnu_iommu_state(void)
{
	uintptr_t m = (uintptr_t)IOMapper::gSystem;

	if (m == 0)
		return XG_DMA_IOMMU_NONE;
	if (m == 1)
		return XG_DMA_IOMMU_UNKNOWN;
	return XG_DMA_IOMMU_ACTIVE;
}
