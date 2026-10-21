/*
 * XeGfx kext entry and PCI personality. For now the module starts the shim only with boot-arg xegfx=1, and the
 * personality never claims the iGPU: a driver that starts on the scanning-out iGPU and is terminated later makes
 * IOPCIFamily reset it on the next match (Legion I-093), so the claim comes only with a driver that keeps it.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <mach/kmod.h>

extern "C" {
void xg_start(void);
void xg_stop(void);
void xg_unimpl(const char *name) __attribute__((__noreturn__));
kern_return_t _start(kmod_info_t *ki, void *d);
kern_return_t _stop(kmod_info_t *ki, void *d);
KMOD_EXPLICIT_DECL(org.watchthelight.XeGfx, "0.0.1", _start, _stop)
}

static bool gShimUp;

extern "C" void
xg_unimpl(const char *name)
{
	panic("XeGfx: %s is not implemented yet", name);
}

extern "C" kern_return_t
_start(kmod_info_t *, void *)
{
	int on = 0;

	if (!PE_parse_boot_argn("xegfx", &on, sizeof(on)) || !on) {
		IOLog("XeGfx: loaded, idle (boot-arg xegfx=1 starts the shim)\n");
		return KERN_SUCCESS;
	}
	xg_start();
	gShimUp = true;
	IOLog("XeGfx: shim started\n");
	return KERN_SUCCESS;
}

extern "C" kern_return_t
_stop(kmod_info_t *, void *)
{
	return gShimUp ? KERN_FAILURE : KERN_SUCCESS;	/* no teardown yet */
}

class XeGfxPCI : public IOService {
	OSDeclareDefaultStructors(XeGfxPCI)
public:
	IOService *probe(IOService *provider, SInt32 *score) override;
};
OSDefineMetaClassAndStructors(XeGfxPCI, IOService)

IOService *
XeGfxPCI::probe(IOService *provider, SInt32 *score)
{
	IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);

	if (pci)
		IOLog("XeGfx: saw %04x:%04x (not claimed in this build)\n", pci->configRead16(kIOPCIConfigVendorID),
		    pci->configRead16(kIOPCIConfigDeviceID));
	return nullptr;
}
