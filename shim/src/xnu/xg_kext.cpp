/*
 * XeGfx kext entry. The module loads idle on IOResources; sysctl debug.xegfx_start=1 starts the shim. There is no PCI
 * personality: a late match on the live iGPU nub makes IOPCIFamily reset its state (getResources, Legion I-094), so the
 * shim finds the GPU in the registry when it starts.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <mach/kmod.h>
#include <sys/sysctl.h>

extern "C" {
void xg_start(void);
void xg_stop(void);
void xg_unimpl(const char *name) __attribute__((__noreturn__));
kern_return_t _start(kmod_info_t *ki, void *d);
kern_return_t _stop(kmod_info_t *ki, void *d);
KMOD_EXPLICIT_DECL(org.watchthelight.XeGfx, "0.0.1", _start, _stop)
}

static bool gShimUp;
static int gState;	/* 0 idle, 1 starting, 2 started */

/* sysctl debug.xegfx_start=1 starts the shim on a running system; a panic then reboots into an idle kext, where a
 * boot-arg would panic again on every boot */
static int
xegfx_start_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	int v = gState, err = sysctl_handle_int(nullptr, &v, 0, req);

	if (err || !req->newptr || v != 1 || gState != 0)
		return err;
	gState = 1;
	IOLog("XeGfx: starting the shim (sysctl)\n");
	xg_start();
	gState = 2;
	gShimUp = true;
	IOLog("XeGfx: shim started\n");
	return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, xegfx_start, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, xegfx_start_sysctl,
    "I", "write 1 to start the XeGfx shim");
SYSCTL_INT(_debug, OID_AUTO, xegfx_state, CTLFLAG_RD | CTLFLAG_LOCKED, &gState, 0, "0 idle, 1 starting, 2 started");

extern "C" void
xg_unimpl(const char *name)
{
	panic("XeGfx: %s is not implemented yet", name);
}

extern "C" kern_return_t
_start(kmod_info_t *, void *)
{
	sysctl_register_oid(&sysctl__debug_xegfx_start);
	sysctl_register_oid(&sysctl__debug_xegfx_state);
	IOLog("XeGfx: loaded, idle (sysctl debug.xegfx_start=1 starts the shim)\n");
	return KERN_SUCCESS;
}

extern "C" kern_return_t
_stop(kmod_info_t *, void *)
{
	if (gShimUp)
		return KERN_FAILURE;	/* no teardown yet */
	sysctl_unregister_oid(&sysctl__debug_xegfx_start);
	sysctl_unregister_oid(&sysctl__debug_xegfx_state);
	return KERN_SUCCESS;
}

/* attaches to IOResources and stays, so the kext is not unloaded while it has no device */
class XeGfxResident : public IOService {
	OSDeclareDefaultStructors(XeGfxResident)
};
OSDefineMetaClassAndStructors(XeGfxResident, IOService)
