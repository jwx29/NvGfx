/*
 * XeGfx kext entry. The module loads idle on IOResources and does nothing until a stage is asked for:
 *   sysctl debug.xegfx_stage=N runs every stage up to N that has not run yet (1 shim, 2 read-only GPU probe,
 *   3 bus and DMA, 4 drm/i915 SYSINITs). debug.xegfx_env sets tunables for the stages (overrides boot-args).
 * Nothing loads it at boot: its one personality waits for the XeGfxArm resource, which _start publishes after a
 * manual `kmutil load -b org.watchthelight.XeGfx`. There is no PCI personality: the shim finds the GPU in the registry. A stage that runs longer than
 * debug.xegfx_wdog seconds panics on purpose, so a stall becomes a panic report and an automatic reboot.
 * <wtl> project: xegfx | author: watchthelight </wtl>
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <architecture/i386/pio.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <mach/kmod.h>
#include <sys/errno.h>
#include <sys/sysctl.h>

extern "C" {
void xg_start_stage(int stage);
int xg_drm_attached(void);
int xg_pci_xnu_start(void *iopcidevice);
void xg_newbus_dump(void);
void xg_unimpl(const char *name) __attribute__((__noreturn__));
kern_return_t _start(kmod_info_t *ki, void *d);
kern_return_t _stop(kmod_info_t *ki, void *d);
KMOD_EXPLICIT_DECL(org.watchthelight.XeGfx, "0.0.5", _start, _stop)
char xg_env[512];
int xg_progress;	/* last breadcrumb a running stage reached; the guard journals it */
}

class XeGfxResident : public IOService {
	OSDeclareDefaultStructors(XeGfxResident)
public:
	bool start(IOService *provider) override;
};

static XeGfxResident *gResident;
static int gStage;	/* highest stage done */
static int gRunning;	/* stage in progress, 0 when idle */
static int gResult[5];	/* per stage: 0 not run, 1 pass, negative errno-style failure */
static int gWdog = 60;
static thread_call_t gWdogCall;
static IOLock *gStageLock;
static uint64_t gSurfPte;
static char gLog[64 * 1024];	/* last 64 KiB of the shim's console output, for sysctl debug.xegfx_log */
static size_t gLogLen;
static IOSimpleLock *gLogLock;
static IOLock *gSnapLock;	/* GGTT entry behind PLANE_SURF_1_A at probe time, 0 if unknown */

bool
XeGfxResident::start(IOService *provider)
{
	if (!IOService::start(provider))
		return false;
	gResident = this;
	registerService();
	return true;
}
OSDefineMetaClassAndStructors(XeGfxResident, IOService)

static void
xg_wdog_fire(thread_call_param_t, thread_call_param_t)
{
	if (gRunning)
		panic("XeGfx: stage %d stalled for %d s at progress %d", gRunning, gWdog, xg_progress);
}

/* ---- stage 2: read-only look at the iGPU and the PCH watchdog, published as XeGfxProbe ---- */

static void
xg_put(OSDictionary *d, const char *k, uint64_t v, int bits = 32)
{
	OSNumber *n = OSNumber::withNumber(v, bits);

	if (n) {
		d->setObject(k, n);
		n->release();
	}
}

static IOPCIDevice *
xg_find_pci(uint16_t vendor, uint8_t cls, uint8_t sub)
{
	IOPCIDevice *found = nullptr;
	OSDictionary *m = IOService::serviceMatching("IOPCIDevice");
	OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;

	OSSafeReleaseNULL(m);
	if (!it)
		return nullptr;
	while (OSObject *o = it->getNextObject()) {
		IOPCIDevice *p = OSDynamicCast(IOPCIDevice, o);
		if (p && p->configRead16(kIOPCIConfigVendorID) == vendor &&
		    p->configRead8(kIOPCIConfigClassCode + 2) == cls && p->configRead8(kIOPCIConfigClassCode + 1) == sub) {
			found = p;
			found->retain();
			break;
		}
	}
	it->release();
	return found;
}

static int
xg_probe(void)
{
	OSDictionary *d = OSDictionary::withCapacity(32);
	IOPCIDevice *gpu = xg_find_pci(0x8086, 0x03, 0x00), *smb = xg_find_pci(0x8086, 0x0c, 0x05);
	int rc = 1;

	if (!d)
		return -12;
	if (!gpu) {
		rc = -19;
		goto out;
	}
	xg_progress = 21;
	xg_put(d, "device-id", gpu->configRead16(kIOPCIConfigDeviceID), 16);
	xg_put(d, "revision", gpu->configRead8(kIOPCIConfigRevisionID), 8);
	xg_put(d, "subsystem", gpu->configRead32(kIOPCIConfigSubSystemVendorID));
	xg_put(d, "command", gpu->configRead16(kIOPCIConfigCommand), 16);
	xg_put(d, "ggc", gpu->configRead16(0x50), 16);
	for (int bar = 0; bar < 2; bar++) {
		IODeviceMemory *m = gpu->getDeviceMemoryWithRegister((UInt8)(kIOPCIConfigBaseAddress0 + bar * 8));
		char k[16];
		snprintf(k, sizeof(k), "bar%d-base", bar * 2);
		xg_put(d, k, m ? m->getPhysicalAddress() : 0, 64);
		snprintf(k, sizeof(k), "bar%d-size", bar * 2);
		xg_put(d, k, m ? m->getLength() : 0, 64);
	}
	/* register reads only, uncached BAR0 map; never BAR2 or stolen memory (Wa_22018444074) */
	if (gpu->configRead16(kIOPCIConfigCommand) & kIOPCICommandMemorySpace) {
		IOMemoryMap *mm = gpu->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
		if (mm) {
			volatile uint32_t *r = (volatile uint32_t *)mm->getVirtualAddress();
			static const struct { const char *k; uint32_t off; } regs[] = {
				{ "gmd-id-graphics", 0xd8c }, { "gmd-id-display", 0x510a0 },
				{ "dsm-base-lo", 0x1080c0 }, { "dsm-base-hi", 0x1080c4 },
				{ "pcode-stolen-access", 0x138914 },
				{ "transconf-a", 0x70008 }, { "pipe-frmcount-a", 0x70040 },
				{ "plane-ctl-1-a", 0x70180 }, { "plane-stride-1-a", 0x70188 }, { "plane-surf-1-a", 0x7019c },
			};
			xg_progress = 22;
			for (auto &e : regs)
				xg_put(d, e.k, r[e.off / 4]);
			if (mm->getLength() >= 16ull << 20) {
				uint32_t surf = r[0x7019c / 4] & ~0xfffu;
				volatile uint64_t *gtt = (volatile uint64_t *)(mm->getVirtualAddress() + (8ull << 20));
				gSurfPte = gtt[surf >> 12];
				xg_put(d, "ggtt-pte-surf", gSurfPte, 64);
			}
			mm->release();
		} else {
			rc = -5;
		}
	}
	xg_progress = 23;
	/* PCH TCO watchdog (SMBus function, TCOBASE at config 0x50, TCOCTL 0x54), I/O port reads only */
	if (smb) {
		uint16_t tcobase = smb->configRead16(0x50) & 0xffe0;
		uint32_t tcoctl = smb->configRead32(0x54);
		xg_put(d, "tco-base", tcobase, 16);
		xg_put(d, "tco-ctl", tcoctl);
		if (tcobase && (tcoctl & (1 << 8))) {
			xg_put(d, "tco-rld", inw(tcobase + 0x0), 16);
			xg_put(d, "tco1-sts", inw(tcobase + 0x4), 16);
			xg_put(d, "tco2-sts", inw(tcobase + 0x6), 16);
			xg_put(d, "tco1-cnt", inw(tcobase + 0x8), 16);
			xg_put(d, "tco2-cnt", inw(tcobase + 0xa), 16);
			xg_put(d, "tco-tmr", inw(tcobase + 0x12), 16);
		}
	}
out:
	xg_put(d, "result", (uint32_t)rc);
	if (gResident)
		gResident->setProperty("XeGfxProbe", d);
	d->release();
	OSSafeReleaseNULL(gpu);
	OSSafeReleaseNULL(smb);
	return rc;
}

/* after i915 has run: the panel must still scan out the boot surface through the same GGTT entry, and pipe A must
 * still count frames; otherwise the desktop is black even though WindowServer looks fine */
static int
xg_display_check(void)
{
	OSDictionary *d = OSDictionary::withCapacity(8);
	IOPCIDevice *gpu = xg_find_pci(0x8086, 0x03, 0x00);
	IOMemoryMap *mm = gpu ? gpu->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache) : nullptr;
	int rc = -6;

	if (mm && d && mm->getLength() >= 16ull << 20) {
		volatile uint32_t *r = (volatile uint32_t *)mm->getVirtualAddress();
		uint32_t surf = r[0x7019c / 4] & ~0xfffu, f0 = r[0x70040 / 4];
		uint64_t pte = ((volatile uint64_t *)(mm->getVirtualAddress() + (8ull << 20)))[surf >> 12];
		IOSleep(100);
		uint32_t f1 = r[0x70040 / 4];
		xg_put(d, "plane-surf-1-a", surf);
		xg_put(d, "ggtt-pte-surf", pte, 64);
		xg_put(d, "frmcount-delta", f1 - f0);
		rc = (gSurfPte && pte != gSurfPte) ? -71 : (f1 == f0 ? -70 : 1);
		xg_put(d, "result", (uint32_t)rc);
		if (gResident)
			gResident->setProperty("XeGfxPost", d);
	}
	OSSafeReleaseNULL(d);
	OSSafeReleaseNULL(mm);
	OSSafeReleaseNULL(gpu);
	return rc;
}

static int
xg_run_stage(int s)
{
	int rc = 1;

	gRunning = s;
	xg_progress = s * 10;
	if (gWdogCall && gWdog > 0) {
		uint64_t dl;
		clock_interval_to_deadline(gWdog, NSEC_PER_SEC, &dl);
		thread_call_enter_delayed(gWdogCall, dl);
	}
	if (s == 2) {
		rc = xg_probe();
	} else {
		xg_start_stage(s);
		if (s == 3) {
			/* every IOPCIDevice into newbus (config reads only), then vgapci on the Intel display function so
			 * stage 4's drm drivers have a device to probe; the old PCI personality used to trigger this */
			IOPCIDevice *gpu = xg_find_pci(0x8086, 0x03, 0x00);
			int err = gpu ? xg_pci_xnu_start(gpu) : ENODEV;
			OSSafeReleaseNULL(gpu);
			rc = err ? -err : 1;
			xg_newbus_dump();
		}
		if (s == 4) {
			rc = xg_display_check();
			if (rc == 1 && !xg_drm_attached())
				rc = -19;	/* the init routines ran but no drm driver bound the GPU */
		}
	}
	if (gWdogCall)
		thread_call_cancel(gWdogCall);
	gRunning = 0;
	return rc;
}

static int
xegfx_stage_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	int v = gStage, err = sysctl_handle_int(nullptr, &v, 0, req);

	if (err || !req->newptr)
		return err;
	if (v < 0 || v > 4)
		return EINVAL;
	IOLockLock(gStageLock);
	while (gStage < v) {
		int s = gStage + 1, rc = xg_run_stage(s);
		gResult[s] = rc;
		if (gResident)
			gResident->setProperty("XeGfxStage", (unsigned long long)s, 32);
		if (rc < 0)
			break;
		gStage = s;
	}
	IOLockUnlock(gStageLock);
	return 0;
}

static int
xegfx_results_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	return SYSCTL_OUT(req, gResult, sizeof(gResult));
}

SYSCTL_PROC(_debug, OID_AUTO, xegfx_stage, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, xegfx_stage_sysctl,
    "I", "highest XeGfx stage done; write N to run stages up to N (1 shim, 2 probe, 3 bus, 4 drm/i915)");
SYSCTL_PROC(_debug, OID_AUTO, xegfx_results, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
    xegfx_results_sysctl, "S", "per-stage result: 0 not run, 1 pass, negative failure");
SYSCTL_INT(_debug, OID_AUTO, xegfx_running, CTLFLAG_RD | CTLFLAG_LOCKED, &gRunning, 0, "stage in progress");
SYSCTL_INT(_debug, OID_AUTO, xegfx_progress, CTLFLAG_RD | CTLFLAG_LOCKED, &xg_progress, 0, "last breadcrumb");
SYSCTL_INT(_debug, OID_AUTO, xegfx_wdog, CTLFLAG_RW | CTLFLAG_LOCKED, &gWdog, 0, "stage stall panic, seconds");
static int xegfx_log_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *);
SYSCTL_PROC(_debug, OID_AUTO, xegfx_log, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, xegfx_log_sysctl,
    "S", "the shim's console output (last 64 KiB)");
SYSCTL_STRING(_debug, OID_AUTO, xegfx_env, CTLFLAG_RW | CTLFLAG_LOCKED, xg_env, sizeof(xg_env),
    "tunables for the stages, name=value pairs");

static struct sysctl_oid *const gOids[] = {
	&sysctl__debug_xegfx_stage, &sysctl__debug_xegfx_results, &sysctl__debug_xegfx_running,
	&sysctl__debug_xegfx_progress, &sysctl__debug_xegfx_wdog, &sysctl__debug_xegfx_env, &sysctl__debug_xegfx_log,
};

extern "C" int
xg_vprintf(const char *fmt, va_list ap)
{
	char buf[512];
	va_list aq;
	int n;

	va_copy(aq, ap);
	n = vsnprintf(buf, sizeof(buf), fmt, aq);
	va_end(aq);
	if (gLogLock && n > 0) {
		size_t l = MIN((size_t)n, sizeof(buf) - 1);
		IOInterruptState is = IOSimpleLockLockDisableInterrupt(gLogLock);
		if (gLogLen + l > sizeof(gLog)) {
			size_t drop = gLogLen + l - sizeof(gLog) + sizeof(gLog) / 4;
			memmove(gLog, gLog + drop, gLogLen - drop);
			gLogLen -= drop;
		}
		memcpy(gLog + gLogLen, buf, l);
		gLogLen += l;
		IOSimpleLockUnlockEnableInterrupt(gLogLock, is);
	}
	return vprintf(fmt, ap);
}

extern "C" int
xg_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = xg_vprintf(fmt, ap);
	va_end(ap);
	return n;
}

static int
xegfx_log_sysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
	static char snap[sizeof(gLog)];
	size_t n;

	IOLockLock(gSnapLock);	/* one reader at a time owns the snapshot; not the stage lock, a stage holds that */
	IOInterruptState is = IOSimpleLockLockDisableInterrupt(gLogLock);
	n = gLogLen;
	memcpy(snap, gLog, n);
	IOSimpleLockUnlockEnableInterrupt(gLogLock, is);
	int err = SYSCTL_OUT(req, snap, n);
	IOLockUnlock(gSnapLock);
	return err;
}

extern "C" void
xg_unimpl(const char *name)
{
	panic("XeGfx: %s is not implemented yet", name);
}

extern "C" kern_return_t
_start(kmod_info_t *, void *)
{
	gStageLock = IOLockAlloc();
	gLogLock = IOSimpleLockAlloc();
	gSnapLock = IOLockAlloc();
	gWdogCall = thread_call_allocate(xg_wdog_fire, nullptr);
	if (!gStageLock || !gWdogCall || !gLogLock || !gSnapLock)
		return KERN_FAILURE;
	/* i915 stays off the display until NMIntelFB owns it; the guard can override per experiment */
	strlcpy(xg_env, "compat.linuxkpi.i915_disable_display=1", sizeof(xg_env));
	for (auto *o : gOids)
		sysctl_register_oid(o);
	IOService::publishResource("XeGfxArm");
	return KERN_SUCCESS;
}

extern "C" kern_return_t
_stop(kmod_info_t *, void *)
{
	if (gStage > 0)
		return KERN_FAILURE;	/* no teardown yet */
	for (auto *o : gOids)
		sysctl_unregister_oid(o);
	thread_call_free(gWdogCall);
	IOLockFree(gStageLock);
	IOSimpleLockFree(gLogLock);
	IOLockFree(gSnapLock);
	return KERN_SUCCESS;
}
