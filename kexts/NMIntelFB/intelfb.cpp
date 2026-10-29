// NMIntelFB: framebuffer for the built-in panel of laptops whose panel is wired to an Intel GPU macOS has no driver for
// (display versions 12-20: Tiger Lake, Rocket Lake, Alder Lake, Raptor Lake, Meteor Lake, Arrow Lake, Lunar Lake).
//
// Phase 1 takes over the mode the firmware already lit: it finds the pipe whose primary plane is scanning out, checks
// that the plane is linear 32-bit XRGB, and publishes that surface (through the graphics aperture, BAR2 + the plane's GGTT
// offset) as an IOFramebuffer with the panel's real size and refresh. Nothing is written to the display engine, so a
// laptop it does not understand keeps exactly the screen it had: start() fails, every reason is logged, and the firmware
// framebuffer (IONDRVFramebuffer) takes the device as before.
//
// Register layout: Linux i915/xe display headers (MIT). Plane 1 of pipe P: PLANE_CTL 0x70180 + P*0x1000, STRIDE +0x08,
// SIZE +0x10, SURF +0x1C. Transcoder T timings at 0x60000 + T*0x1000, TRANSCONF at 0x70008 + P*0x1000, PIPESRC 0x6001C.

#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/OSAtomic.h>
#include "nvrm_vram_abi.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread_call.h>
#include <sys/sysctl.h>

#define IFBLOG(fmt, ...) IOLog("NMIntelFB: " fmt "\n", ##__VA_ARGS__)

// Boot guard, as NVRM's nvrm_off.h: each start counts in NVRAM, WindowServer going active clears it, two counted
// starts in a row turn the driver off so the firmware framebuffer gives the next boot a desktop.
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#define NMIFB_GUARD_VAR "nmintelfb-starts"
static unsigned nmifbGuardStarts(IORegistryEntry *o)
{
    OSObject *v = o->copyProperty(NMIFB_GUARD_VAR); OSData *d = OSDynamicCast(OSData, v);
    unsigned n = (d && d->getLength() >= 1) ? *(const unsigned char *)d->getBytesNoCopy() : 0;
    if (v) v->release();
    return n;
}
static bool nmifbGuardTripped()
{
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return false;
    bool t = nmifbGuardStarts(o) >= 2; o->release(); return t;
}
static void nmifbGuardCount()
{
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return;
    unsigned char n = (unsigned char)(nmifbGuardStarts(o) + 1);
    if (OSData *d = OSData::withBytes(&n, 1)) { if (o->setProperty(NMIFB_GUARD_VAR, d)) IFBLOG("boot guard: start %u without WindowServer so far (off after 2)", n); d->release(); }
    o->release();
}
// Step breadcrumb (I-090: a hang that left no log): each stage writes "nmintelfb-step" and syncs NVRAM at once, so the
// next boot reads the last stage reached with `nvram nmintelfb-step`. Boot-arg nmintelfbstop=N declines at stage N.
#include <IOKit/IONVRAM.h>
static bool nmifbStep(unsigned n, const char *tag)
{
    char b[64]; snprintf(b, sizeof b, "%u %s", n, tag);
    IFBLOG("step %s", b);
    if (IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane)) {
        static bool kept;   // the boot after a hang overwrites the marker, so the old one moves to -prev first
        if (!kept) { kept = true; if (OSObject *old = o->copyProperty("nmintelfb-step")) { o->setProperty("nmintelfb-step-prev", old); old->release(); } }
        if (OSData *d = OSData::withBytes(b, (unsigned)strlen(b))) { o->setProperty("nmintelfb-step", d); d->release(); }
        if (IODTNVRAM *nv = OSDynamicCast(IODTNVRAM, o)) nv->sync();
        o->release();
    }
    unsigned stop = 0;
    if (PE_parse_boot_argn("nmintelfbstop", &stop, sizeof stop) && stop == n) { IFBLOG("stopping at step %u by boot-arg", n); return true; }
    return false;
}
static void nmifbGuardClear()
{
    static bool done; if (done) return; done = true;
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return;
    o->removeProperty(NMIFB_GUARD_VAR); o->release(); IFBLOG("boot guard cleared: WindowServer is active on the panel");
}

// kmutil places a kext in a kernel collection only with a _kmod_info symbol (Xcode generates it; this build does not)
#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
#ifdef NMINTELFB_VIRTUAL
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMIntelFBVirtual, "0.1", _start, _stop) }
#else
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMIntelFB, "0.3", _start, _stop) }
#endif
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

namespace {
// display version by PCI device ID, from Linux include/drm/intel/pciids.h (MIT); 0 = not ours
struct Id { uint16_t id; uint8_t ver; };
const Id kIds[] = {
#include "display_ids.inc"
};

uint8_t displayVersion(uint16_t dev)
{
    for (const Id &e : kIds) if (e.id == dev) return e.ver;
    return 0;
}

constexpr uint32_t PLANE_CTL(int p)    { return 0x70180u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_STRIDE(int p) { return 0x70188u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_SIZE(int p)   { return 0x70190u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_SURF(int p)   { return 0x7019Cu + (uint32_t)p * 0x1000u; }
constexpr uint32_t TRANSCONF(int p)    { return 0x70008u + (uint32_t)p * 0x1000u; }
constexpr uint32_t HTOTAL(int t)       { return 0x60000u + (uint32_t)t * 0x1000u; }
constexpr uint32_t VTOTAL(int t)       { return 0x6000Cu + (uint32_t)t * 0x1000u; }
constexpr uint32_t HBLANK(int t)       { return 0x60004u + (uint32_t)t * 0x1000u; }
constexpr uint32_t HSYNC(int t)        { return 0x60008u + (uint32_t)t * 0x1000u; }
constexpr uint32_t VBLANK(int t)       { return 0x60010u + (uint32_t)t * 0x1000u; }
constexpr uint32_t VSYNC(int t)        { return 0x60014u + (uint32_t)t * 0x1000u; }
// one count per vblank (Linux PIPE_FRMCOUNT_G4X, 0x70040 + pipe): counting it is how the real refresh is known without
// reading the PLL and M/N values back
constexpr uint32_t FRMCOUNT(int p)     { return 0x70040u + (uint32_t)p * 0x1000u; }

// PLANE_CTL (display 11+): enable bit 31, format bits 27:23 (8 = XRGB 8:8:8:8, Linux PLANE_CTL_FORMAT_XRGB_8888 on
// ICL+), tiling bits 12:10 (0 = linear)
constexpr uint32_t kEnable = 1u << 31;
// the two power states NVRMFB registers: off, and on + usable
IOPMPowerState kPowerStates[2] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};
constexpr uint32_t fmtOf(uint32_t ctl)  { return (ctl >> 23) & 0x1F; }
constexpr uint32_t tileOf(uint32_t ctl) { return (ctl >> 10) & 0x7; }
}

class NMIntelFB : public IOFramebuffer {
    OSDeclareDefaultStructors(NMIntelFB)
    IOPCIDevice *fPCI = nullptr;
    IOMemoryMap *fMMIO = nullptr;
    IOMemoryMap *fSurfMap = nullptr;   // write-combined CPU view of the scan-out surface (NVIDIA frames land here)
    // WindowServer paces an accelerated display on its VBL interrupt: the NVIDIA display pipe completes each frame on it.
    // Without one the first frame never completed and WindowServer stalled every display (in testing, the
    // virtual panel froze the Mac). A 60 Hz timer is the VBL, as NVRMFB's fallback is.
    struct Irq { IOFBInterruptProc proc = nullptr; OSObject *target = nullptr; void *ref = nullptr; };
    Irq fVbl, fConnect;
    thread_call_t fVblTimer = nullptr;
    uint64_t fVblNext = 0, fVblCalls = 0;
    volatile bool fStopping = false;
    static void vblFire(thread_call_param_t p0, thread_call_param_t) {
        NMIntelFB *me = (NMIntelFB *)p0;
        if (me->fStopping) return;
        if (me->fVbl.proc) { me->fVbl.proc(me->fVbl.target, me->fVbl.ref); me->fVblCalls++; }
        uint64_t period; nanoseconds_to_absolutetime(me->fFrameNs, &period);
        uint64_t now = mach_absolute_time();
        me->fVblNext = (me->fVblNext && me->fVblNext + period > now) ? me->fVblNext + period : now + period;
        thread_call_enter_delayed(me->fVblTimer, me->fVblNext);
    }
    void armVbl() {
        fVblTimer = thread_call_allocate(vblFire, this);
        if (fVblTimer) { uint64_t d; clock_interval_to_deadline((uint32_t)(fFrameNs / 1000), kMicrosecondScale, &d); fVblNext = d; thread_call_enter_delayed(fVblTimer, d); }
        IFBLOG("vbl timer %s", fVblTimer ? "armed (60 Hz)" : "ALLOCATE FAILED");
    }
#ifdef NMINTELFB_VIRTUAL
    IOBufferMemoryDescriptor *fVBuf = nullptr;
    IOTimerEventSource *fTimer = nullptr;
    unsigned fSamples = 0;
    // 64 pixels across the buffer: how many are non-black and how many differ from the first, published in the registry
public:
    // 64 pixels across the buffer, read on demand through sysctl debug.nmintelfb_sample (the registry property the
    // timer set never showed up)
    void sampleInto(char *b, size_t n) {
        if (!fSurfMap) { snprintf(b, n, "no surface"); return; }
        const volatile uint32_t *px = (const volatile uint32_t *)fSurfMap->getVirtualAddress();
        unsigned nz = 0, varied = 0; uint32_t first = px[0] & 0xFFFFFF;
        for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 8; j++) {
            uint32_t v = px[(fHeight * (2 * i + 1) / 16) * (fRowBytes / 4) + fWidth * (2 * j + 1) / 16] & 0xFFFFFF;
            if (v) nz++; if (v != first) varied++; }
        snprintf(b, n, "sample %u: nonzero %u/64 varied %u/64 first 0x%06x", ++fSamples, nz, varied, first);
    }
    void sample() { char b[96]; sampleInto(b, sizeof b); setProperty("NMIntelFBSample", b); }
private:
#endif   // write-combined CPU view of the scan-out surface (NVIDIA frames land here)
    volatile uint8_t *fRegs = nullptr;
    IOPhysicalAddress fApertureBase = 0;
    IOPhysicalLength fApertureLen = 0;
    uint32_t fWidth = 0, fHeight = 0, fRowBytes = 0, fSurf = 0, fRefresh16 = 60 << 16;
    uint64_t fFrameNs = 16666667ull;   // vblank period; measured from the pipe's frame counter on real hardware
    // transcoder timing (active, total, sync start/end) for getTimingInfoForDisplayMode
    uint32_t fHT = 0, fVT = 0, fHSS = 0, fHSE = 0, fVSS = 0, fVSE = 0;
    uint64_t fPclk = 0;
    int fPipe = -1;
    uint8_t fVer = 0;

    uint32_t rd(uint32_t off) const { return *(volatile uint32_t *)(fRegs + off); }
    bool adoptFirmwareMode();
    // nmintelfbcache=1 write-combined, 2 cache-inhibited, else IOKit's default for device memory
    void mapSurface() {
        unsigned c = 0; PE_parse_boot_argn("nmintelfbcache", &c, sizeof c);
        IOOptionBits mode = c == 1 ? kIOMapWriteCombineCache : c == 2 ? kIOMapInhibitCache : kIOMapDefaultCache;
        nmifbStep(8, c == 1 ? "map surface wc" : c == 2 ? "map surface uc" : "map surface default");
        if (IODeviceMemory *sm = IODeviceMemory::withRange(fApertureBase + fSurf, (IOPhysicalLength)fRowBytes * fHeight)) {
            fSurfMap = sm->map(mode); sm->release();
        }
        IFBLOG("surface map %s", fSurfMap ? "made" : "FAILED: NVIDIA rendering stays off for this panel");
        if (fSurfMap) nmifbStep(13, "surface mapped");
    }

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    IOReturn setProperties(OSObject *properties) override;
    void stop(IOService *provider) override;

    IOReturn enableController() override { return kIOReturnSuccess; }
    bool isConsoleDevice() override { return true; }
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    const char *getPixelFormats() override { return IO32BitDirectPixels "\0"; }
    IOItemCount getDisplayModeCount() override { return 1; }
    IOReturn getDisplayModes(IODisplayModeID *modes) override { modes[0] = 1; return kIOReturnSuccess; }
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { *mode = 1; *depth = 0; return kIOReturnSuccess; }
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { return getCurrentDisplayMode(mode, depth); }
    // WindowServer showed the panel at 0.00 Hz without this (virtual panel)
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
        if (mode != 1 || !info) return kIOReturnUnsupportedMode;
        bzero(info, sizeof(*info));
        info->appleTimingID = kIOTimingIDInvalid;
        if (!fPclk || fHT <= fWidth || fVT <= fHeight) return kIOReturnSuccess;
        IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
        d.pixelClock = d.minPixelClock = d.maxPixelClock = fPclk;
        d.horizontalActive = fWidth; d.horizontalBlanking = fHT - fWidth;
        d.horizontalSyncOffset = fHSS > fWidth ? fHSS - fWidth : 0; d.horizontalSyncPulseWidth = fHSE > fHSS ? fHSE - fHSS : 0;
        d.verticalActive = fHeight; d.verticalBlanking = fVT - fHeight;
        d.verticalSyncOffset = fVSS > fHeight ? fVSS - fHeight : 0; d.verticalSyncPulseWidth = fVSE > fVSS ? fVSE - fVSS : 0;
        info->flags = kIODetailedTimingValid;
        return kIOReturnSuccess;
    }
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override { return (mode == 1 && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupported; }
    IOItemCount getConnectionCount() override { return 1; }
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override;
    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **iref) override {
        if (type == kIOFBVBLInterruptType) { fVbl = { proc, target, ref }; if (iref) *iref = &fVbl; return kIOReturnSuccess; }
        if (type == kIOFBConnectInterruptType) { fConnect = { proc, target, ref }; if (iref) *iref = &fConnect; return kIOReturnSuccess; }
        return kIOReturnUnsupported;
    }
    IOReturn unregisterInterrupt(void *) override { return kIOReturnSuccess; }
    IOReturn setInterruptState(void *, UInt32) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *, bool) override { return kIOReturnSuccess; }
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
    // Power and connection handling copied from NVRMFB, which shares this controller (same IOFBDependentID). Left to
    // IOFramebuffer's base class, a power change queues on the shared controller thread and waits for an acknowledge
    // (IOFramebuffer::setPowerState returns a 45 s timeout, checkPowerWork -> setAttribute(kIOPowerStateAttribute));
    // NVRMFB acknowledges at once and answers the power attributes itself. With NMIntelFB under NVRM the
    // first WindowServer restart froze every display.
    IOReturn setPowerState(unsigned long ordinal, IOService *) override {
        static bool once[2]; if (ordinal < 2 && !once[ordinal]) { once[ordinal] = true; nmifbStep(10 + (unsigned)ordinal, ordinal ? "power on" : "power off"); }
        handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
        return kIOPMAckImplied;
    }
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override {
        if (attribute == kIOWindowServerActiveAttribute) { if (value) { nmifbGuardClear(); nmifbStep(12, "windowserver active"); } return kIOReturnSuccess; }
        if (attribute == kIOPowerAttribute) {
            if (value) { IOReturn r = IOFramebuffer::setAttribute(attribute, value); handleEvent(kIOFBNotifyDidPowerOn);
                         return r == kIOReturnUnsupported ? kIOReturnSuccess : r; }
            handleEvent(kIOFBNotifyWillPowerOff);
            IOReturn r = IOFramebuffer::setAttribute(attribute, value);
            return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
        }
        return IOFramebuffer::setAttribute(attribute, value);
    }
    IOReturn setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value) override {
        switch (attribute) {
        case kConnectionPower: case kConnectionColorModesSupported: case kConnectionColorDepthsSupported:
        case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported: case kConnectionColorMode:
        case kConnectionDisplayFlags: case kConnectionFlags:
            return kIOReturnSuccess;
        default: return IOFramebuffer::setAttributeForConnection(idx, attribute, value);
        }
    }
    IOReturn connectFlags(IOIndex, IODisplayModeID, IOOptionBits *flags) override {
        if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
        return kIOReturnSuccess;
    }
    IODeviceMemory *getVRAMRange() override { return getApertureRange(kIOFBSystemAperture); }
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override {
        if (attribute == kIOWindowServerActiveAttribute) { if (value) *value = 1; return kIOReturnSuccess; }
        return IOFramebuffer::getAttribute(attribute, value);
    }
    IOReturn getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value) override;
};

OSDefineMetaClassAndStructors(NMIntelFB, IOFramebuffer)

/* Reporting-only: root passes ANV's queried shared heap, never a scanout map. */
IOReturn NMIntelFB::setProperties(OSObject *properties)
{
    auto outer = OSDynamicCast(OSDictionary, properties);
    OSObject *payload = outer ? outer->getObject("XeGfxGPUIdentity") : nullptr;
    if (!payload) return IOFramebuffer::setProperties(properties);

    if (IOUserClient::clientHasPrivilege(current_task(),
            kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;

    auto report = OSDynamicCast(OSDictionary, payload);
    if (!report || !fPCI || fStopping || !fWidth || !fHeight ||
        fPCI->configRead16(kIOPCIConfigVendorID) != 0x8086 ||
        fPCI->configRead16(kIOPCIConfigDeviceID) != 0x7d55)
        return kIOReturnNotReady;

    auto number = [report](const char *key) {
        return OSDynamicCast(OSNumber, report->getObject(key));
    };
    OSNumber *version = number("version");
    OSNumber *vendor = number("vendor");
    OSNumber *device = number("device");
    OSNumber *heap = number("sharedHeapBytes");
    OSNumber *ram = number("physicalRAMBytes");
    OSString *name = OSDynamicCast(OSString, report->getObject("name"));
    if (!version || !vendor || !device || !heap || !ram || !name ||
        version->unsigned64BitValue() != 1 ||
        vendor->unsigned64BitValue() != 0x8086 ||
        device->unsigned64BitValue() != 0x7d55)
        return kIOReturnBadArgument;

    const uint64_t bytes = heap->unsigned64BitValue();
    const uint64_t physical = ram->unsigned64BitValue();
    if (bytes < (256ull << 20) || !physical ||
        physical > (1ull << 40) || bytes > physical ||
        (bytes >> 20) > 0xffffffffull ||
        !name->getLength() || name->getLength() > 127)
        return kIOReturnBadArgument;
    const char *label = name->getCStringNoCopy();
    if (!label || label[name->getLength()] != '\0')
        return kIOReturnBadArgument;
    for (unsigned i = 0; i < name->getLength(); ++i)
        if ((unsigned char)label[i] < 0x20 ||
            (unsigned char)label[i] > 0x7e)
            return kIOReturnBadArgument;

    OSData *model = OSData::withBytes(label, name->getLength() + 1);
    OSNumber *mb = OSNumber::withNumber(bytes >> 20, 32);
    if (!model || !mb) {
        if (model) model->release();
        if (mb) mb->release();
        return kIOReturnNoMemory;
    }

    /* The PCI property is read by System Information on Intel shared GPUs. */
    bool ok = fPCI->setProperty("model", model);
    ok = fPCI->setProperty("VRAM,totalMB", mb) && ok;
    ok = fPCI->setProperty("XeGfxSharedHeapBytes", heap) && ok;
    ok = fPCI->setProperty("XeGfxMemoryIsShared", kOSBooleanTrue) && ok;
    /* Also expose the values on the framebuffer for straightforward checks. */
    ok = setProperty("VRAM,totalMB", mb) && ok;
    ok = setProperty("XeGfxSharedHeapBytes", heap) && ok;
    model->release();
    mb->release();
    IFBLOG("shared GPU identity: %s, heap=%llu MiB, published=%d",
           label, (unsigned long long)(bytes >> 20), ok);
    return ok ? kIOReturnSuccess : kIOReturnNoMemory;
}

#ifdef NMINTELFB_VIRTUAL
static NMIntelFB *gVirt;
static int nmintelfb_sample_sysctl SYSCTL_HANDLER_ARGS
{
    char b[128] = "no panel";
    if (gVirt) gVirt->sampleInto(b, sizeof b);
    return sysctl_handle_string(oidp, b, sizeof b, req);
}
SYSCTL_PROC(_debug, OID_AUTO, nmintelfb_sample, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0,
            nmintelfb_sample_sysctl, "A", "64 pixels of the virtual panel: non-black and varied counts");
#endif

IOService *NMIntelFB::probe(IOService *provider, SInt32 *score)
{
#ifdef NMINTELFB_VIRTUAL
    if (!provider->getProperty("nm-intel-panel")) return nullptr;   // only the nub the loader published
    // test build: a virtual 1920x1080 "panel" in system memory, so the NVIDIA -> Intel frame path can be proven on a
    // machine with no Intel GPU (frames composited on the NVIDIA card must arrive in this buffer)
    fVer = 0xFF; return IOFramebuffer::probe(provider, score);
#endif
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return nullptr;
    if (PE_parse_boot_argn("-nmintelfboff", nullptr, 0)) { IFBLOG("off by boot-arg"); return nullptr; }
    uint16_t dev = pci->configRead16(kIOPCIConfigDeviceID);
    fVer = displayVersion(dev);
    if (!fVer) return nullptr;   // an Intel GPU this driver does not know (older ones have Apple's own driver)
    if (nmifbStep(1, "probe")) return nullptr;
    if (nmifbGuardTripped()) { IFBLOG("off: two starts in a row never reached WindowServer (clear with: sudo nvram -d " NMIFB_GUARD_VAR ")"); return nullptr; }
    IFBLOG("8086:%04x display version %u", dev, fVer);
    return IOFramebuffer::probe(provider, score);
}

bool NMIntelFB::adoptFirmwareMode()
{
    // Firmware can light both eDP and HDMI before macOS starts. Do not blindly
    // adopt the first enabled pipe: it may not be the GOP/boot-console surface.
    // Only select a pipe whose validated linear plane matches the firmware
    // console's geometry. This is firmware HDMI-primary support, not hotplug
    // or HDMI link/mode training; it is safe to leave all display registers alone.
    PE_Video console; bzero(&console, sizeof console);
    const bool haveConsole = getPlatform() &&
        getPlatform()->getConsoleInfo(&console) == kIOReturnSuccess &&
        console.v_baseAddr && console.v_depth == 32;
    const uint32_t diagnosticDDI = rd(0xC4030u); // SHOTPLUG_CTL_DDI, read-only
    const uint32_t diagnosticTC = rd(0xC4034u);  // SHOTPLUG_CTL_TC, read-only
    setProperty("NMIntelFBHPDDDI", diagnosticDDI, 32);
    setProperty("NMIntelFBHPDTC", diagnosticTC, 32);
    IFBLOG("read-only HPD snapshot: SHOTPLUG_CTL_DDI=0x%08x TC=0x%08x (status bits are not proof of HDMI routing)",
           diagnosticDDI, diagnosticTC);
    if (!haveConsole) {
        IFBLOG("no validated 32-bit firmware console; refusing to guess the active HDMI/eDP pipe");
        return false;
    }
    int candidates = 0, chosen = -1;
    for (int p = 0; p < 4; p++) {
        const uint32_t conf = rd(TRANSCONF(p)), ctl = rd(PLANE_CTL(p));
        if (!(conf & kEnable) || !(ctl & kEnable)) continue;
        const uint32_t size = rd(PLANE_SIZE(p)), stride = rd(PLANE_STRIDE(p)) & 0x7FFu;
        const uint32_t width = (size & 0xFFFFu) + 1u, height = (size >> 16) + 1u, pitch = stride * 64u;
        const bool linear = fmtOf(ctl) == 8 && tileOf(ctl) == 0;
        const bool match = linear && width >= 320u && height >= 200u && pitch >= width * 4u &&
            width == console.v_width && height == console.v_height && pitch == console.v_rowBytes;
        IFBLOG("firmware pipe %c: enabled plane %ux%u pitch %u format %u tile %u console-match %d",
               'A' + p, width, height, pitch, fmtOf(ctl), tileOf(ctl), match ? 1 : 0);
        if (match) { chosen = p; ++candidates; }
    }
    // No match must fail closed. If several pipes use identical geometry,
    // preserve the earlier first-pipe policy for compatibility but report the
    // ambiguity: only a true GGTT/console identity check can distinguish them.
    if (!candidates) {
        IFBLOG("no lit linear plane matches the boot console; HDMI needs firmware GOP takeover or native modesetting");
        return false;
    }
    if (candidates > 1) {
        IFBLOG("%d lit planes match console geometry; selecting first as before (cannot disambiguate eDP/HDMI from size alone)", candidates);
        for (int p = 0; p < 4; ++p) {
            uint32_t ctl = rd(PLANE_CTL(p));
            if ((rd(TRANSCONF(p)) & kEnable) && (ctl & kEnable)) { chosen = p; break; }
        }
    }
    setProperty("NMIntelFBFirmwarePipeMatches", candidates, 32);
    for (int p = chosen; p <= chosen; ++p) {
        if (nmifbStep(3, "plane regs")) return false;
        uint32_t size = rd(PLANE_SIZE(p)), stride = rd(PLANE_STRIDE(p)) & 0x7FF;
        fWidth = (size & 0xFFFF) + 1; fHeight = ((size >> 16) & 0xFFFF) + 1;
        fRowBytes = stride * 64;
        fSurf = rd(PLANE_SURF(p)) & ~0xFFFu;
        if (fWidth < 320 || fHeight < 200 || fRowBytes < fWidth * 4) return false;
        if ((uint64_t)fSurf + (uint64_t)fRowBytes * fHeight > fApertureLen)   // only informative: BAR2 is not used for scanout
            IFBLOG("pipe %c: surface at GGTT 0x%x lies past the %llu MiB BAR2 window", 'A' + p, fSurf, (unsigned long long)(fApertureLen >> 20));
        // the transcoder that feeds pipe p is p on these display versions (eDP on transcoder A in every recording)
        if (nmifbStep(4, "timing regs")) return false;
        fHT = (rd(HTOTAL(p)) >> 16) + 1; fVT = (rd(VTOTAL(p)) >> 16) + 1;
        fHSS = (rd(HSYNC(p)) & 0xFFFF) + 1; fHSE = (rd(HSYNC(p)) >> 16) + 1;
        fVSS = (rd(VSYNC(p)) & 0xFFFF) + 1; fVSE = (rd(VSYNC(p)) >> 16) + 1;
        // refresh: count vblanks for 250 ms (a 30-240 Hz panel gives 7-60 counts); fall back to 60 Hz if it does not move
        if (nmifbStep(5, "frame counter")) return false;
        uint32_t f0 = rd(FRMCOUNT(p)); uint64_t t0 = mach_absolute_time();
        IOSleep(250);
        uint32_t f1 = rd(FRMCOUNT(p)); uint64_t t1 = mach_absolute_time(), ns = 0;
        absolutetime_to_nanoseconds(t1 - t0, &ns);
        uint32_t frames = f1 - f0;
        if (frames >= 5 && frames <= 100 && ns) {
            fFrameNs = ns / frames;
            uint64_t mhz = (uint64_t)frames * 1000000000000ull / ns;   // refresh in milli-Hz
            fRefresh16 = (uint32_t)((mhz << 16) / 1000);
            fPclk = (uint64_t)fHT * fVT * mhz / 1000;
        } else fPclk = (uint64_t)fHT * fVT * 60;
        fPipe = p;
        IFBLOG("pipe %c: %ux%u stride %u surface 0x%x totals %ux%u, %u vblanks in %llu ms -> %u.%02u Hz, pclk %llu", 'A' + p,
               fWidth, fHeight, fRowBytes, fSurf, fHT, fVT, frames, (unsigned long long)(ns / 1000000),
               fRefresh16 >> 16, ((fRefresh16 & 0xFFFF) * 100) >> 16, (unsigned long long)fPclk);
        return true;
    }
    IFBLOG("no pipe is scanning out - the firmware left the panel off; leaving it to the firmware framebuffer");
    return false;
}

bool NMIntelFB::start(IOService *provider)
{
#ifdef NMINTELFB_VIRTUAL
    fWidth = 1920; fHeight = 1080; fRowBytes = 7680; fSurf = 0; fPipe = 0;
    fHT = 2200; fVT = 1125; fHSS = 2008; fHSE = 2052; fVSS = 1084; fVSE = 1089; fPclk = 148500000ull;   // CEA 1080p60
    fVBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                             (mach_vm_size_t)fRowBytes * fHeight, 0xFFFFFFFFFFFFF000ull);
    if (!fVBuf || fVBuf->prepare() != kIOReturnSuccess) { IFBLOG("virtual: no contiguous buffer"); OSSafeReleaseNULL(fVBuf); return false; }
    bzero(fVBuf->getBytesNoCopy(), fVBuf->getLength());
    fApertureBase = fVBuf->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone); fApertureLen = fVBuf->getLength();
    // IOFramebuffer must see the controller/group metadata during start().
    setProperty("IOFBDependentID", 0x4E56524D00000001ull, 64);
    setProperty("IOFBDependentIndex", 0ull, 32);
    if (!IOFramebuffer::start(provider)) { fVBuf->complete(); OSSafeReleaseNULL(fVBuf); return false; }
    fSurfMap = fVBuf->map();
    if (IOWorkLoop *wl = getWorkLoop()) {
        fTimer = IOTimerEventSource::timerEventSource(this, [](OSObject *o, IOTimerEventSource *t) {
            ((NMIntelFB *)o)->sample(); t->setTimeoutMS(2000); });
        if (fTimer && wl->addEventSource(fTimer) == kIOReturnSuccess) fTimer->setTimeoutMS(2000);
    }
    armVbl();
    registerPowerDriver(this, kPowerStates, 2);
    gVirt = this; sysctl_register_oid(&sysctl__debug_nmintelfb_sample);
    IFBLOG("virtual panel published (1920x1080 at phys 0x%llx)", (unsigned long long)fApertureBase);
    return true;
#endif
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) return false;
    if (nmifbStep(2, "map bar0")) return false;
    fPCI->setMemoryEnable(true);
    fMMIO = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    IODeviceMemory *ap = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!fMMIO || !ap) { IFBLOG("BAR0 or BAR2 is not mapped - leaving the firmware screen"); OSSafeReleaseNULL(fMMIO); return false; }
    fRegs = (volatile uint8_t *)fMMIO->getVirtualAddress();
    fApertureBase = ap->getPhysicalAddress(); fApertureLen = ap->getLength();
    if (!adoptFirmwareMode()) { OSSafeReleaseNULL(fMMIO); fRegs = nullptr; return false; }
    // Scan out at the boot console's address (what IONDRV uses), never BAR2 + PLANE_SURF: on display 14 CPU access to
    // stolen memory through the BAR can hang the machine (Linux i915 Wa_22018444074).
    {
        if (nmifbStep(6, "console info")) { OSSafeReleaseNULL(fMMIO); fRegs = nullptr; return false; }
        PE_Video v; bzero(&v, sizeof v);
        bool cons = getPlatform() && getPlatform()->getConsoleInfo(&v) == kIOReturnSuccess && v.v_baseAddr && v.v_depth == 32;
        uint64_t cbase = cons ? ((uint64_t)v.v_baseAddr & ~3ull) : 0;
        IFBLOG("scanout: console 0x%llx %lux%lu rowBytes %lu, BAR2 0x%llx + PLANE_SURF 0x%x = 0x%llx (not used)",
               (unsigned long long)cbase, cons ? v.v_width : 0, cons ? v.v_height : 0, cons ? v.v_rowBytes : 0,
               (unsigned long long)fApertureBase, fSurf, (unsigned long long)(fApertureBase + fSurf));
        if (!cons || v.v_width != fWidth || v.v_height != fHeight || v.v_rowBytes != fRowBytes) {
            IFBLOG("the boot console does not match the lit plane - leaving the firmware screen");
            OSSafeReleaseNULL(fMMIO); fRegs = nullptr; return false;
        }
        fApertureBase = cbase; fSurf = 0; fApertureLen = (IOPhysicalLength)fRowBytes * fHeight;
    }
    if (nmifbStep(7, "framebuffer start")) { OSSafeReleaseNULL(fMMIO); fRegs = nullptr; return false; }
    nmifbGuardCount();   // only a start that is about to publish can fail WindowServer; a clean decline is not a strike
    // Publish the controller/group metadata BEFORE IOFramebuffer::start(), as IOGraphics
    // can establish framebuffer grouping while start() is running.
    setProperty("IOFBDependentID", 0x4E56524D00000001ull, 64);
    setProperty("IOFBDependentIndex", 0ull, 32);
    if (!IOFramebuffer::start(provider)) { OSSafeReleaseNULL(fMMIO); fRegs = nullptr; return false; }
    // Head 4 of the NVIDIA accelerator (NVAccel): WindowServer composites this panel on the NVIDIA card and the
    // display pipe copies each frame into fSurfMap. Without NVAccel the panel stays a plain framebuffer.
    // The kernel mapping of the surface is made only when NVAccel asks for the scanout (head 4 copy path): a
    // write-combined map of the DSM framebuffer in start() hung the machine twice (I-090, stage 8).
    setProperty("NMIntelFBPipe", fPipe, 32);
    setProperty("NMIntelFBDisplayVersion", fVer, 32);

    // Publish the firmware's real timing for XeGfxAGDC K5.
    // Format: 13 x uint32_t, consumed by nvrm-agdc-k5.h.
    if (fPclk > 0 && fPclk <= 0xFFFFFFFFull &&
        fHT > fWidth && fVT > fHeight &&
        fHSS >= fWidth && fHSS < fHSE && fHSE <= fHT &&
        fVSS >= fHeight && fVSS < fVSE && fVSE <= fVT) {

        const uint32_t timing[13] = {
            0x4B350001u,                    // NVRMTiming tag
            static_cast<uint32_t>(fPclk),  // pixel clock, Hz
            fWidth, fHSS, fHSE, fHT,       // horizontal
            fHeight, fVSS, fVSE, fVT,      // vertical
            static_cast<uint32_t>(
                fPclk * 1000ull /
                (static_cast<uint64_t>(fHT) * fVT)
            ),                              // refresh, milli-Hz
        0, 0                            // sync polarity (unknown)
    };

    OSData *data = OSData::withBytes(timing, sizeof(timing));
    if (data) {
        setProperty("NVRMTiming", data);
        data->release();

        IFBLOG("NVRMTiming published: %ux%u, "
               "totals %ux%u, pclk %llu",
               fWidth, fHeight, fHT, fVT,
               (unsigned long long)fPclk);
    }
} else {
    IFBLOG("NVRMTiming not published: "
           "invalid/incomplete firmware timing");
}

    setProperty("built-in", kOSBooleanTrue);   // the panel is the laptop's own (kConnectionFlags says so too)
    armVbl();
    registerPowerDriver(this, kPowerStates, 2);
    IFBLOG("built-in panel published (%ux%u)", fWidth, fHeight);
    nmifbStep(9, "published");
    return true;
}

void NMIntelFB::stop(IOService *provider)
{
    fStopping = true;
#ifdef NMINTELFB_VIRTUAL
    if (gVirt == this) { sysctl_unregister_oid(&sysctl__debug_nmintelfb_sample); gVirt = nullptr; }
#endif
    // thread_call_cancel_wait is not exported to kexts: cancel and retry the free until the callback is out (as NVRMFB)
    if (fVblTimer) { thread_call_cancel(fVblTimer); while (!thread_call_free(fVblTimer)) { thread_call_cancel(fVblTimer); IOSleep(5); } fVblTimer = nullptr; }
    OSSafeReleaseNULL(fSurfMap); OSSafeReleaseNULL(fMMIO); fRegs = nullptr;
    IOFramebuffer::stop(provider);
}

IODeviceMemory *NMIntelFB::getApertureRange(IOPixelAperture aperture)
{
    if (aperture != kIOFBSystemAperture) return nullptr;
    return IODeviceMemory::withRange(fApertureBase + fSurf, (IOPhysicalLength)fRowBytes * fHeight);
}

IOReturn NMIntelFB::getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info)
{
    if (mode != 1 || !info) return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->maxDepthIndex = 0; info->nominalWidth = fWidth; info->nominalHeight = fHeight;
    info->refreshRate = fRefresh16; info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
    return kIOReturnSuccess;
}

IOReturn NMIntelFB::getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi)
{
    if (mode != 1 || depth != 0 || aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
    bzero(pi, sizeof(*pi));
    pi->bytesPerRow = fRowBytes; pi->bytesPerPlane = 0; pi->bitsPerPixel = 32; pi->pixelType = kIORGBDirectPixels;
    pi->componentCount = 3; pi->bitsPerComponent = 8;
    pi->componentMasks[0] = 0x00FF0000; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x000000FF;
    strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
    pi->activeWidth = fWidth; pi->activeHeight = fHeight;
    return kIOReturnSuccess;
}

IOReturn NMIntelFB::getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value)
{
    if (idx != 0 || !value) return kIOReturnBadArgument;
    switch (attr) {
    case kConnectionEnable: *value = 1; return kIOReturnSuccess;
    case kConnectionCheckEnable: *value = 1; return kIOReturnSuccess;
    case kConnectionFlags: *value = kIOConnectionBuiltIn; return kIOReturnSuccess;
    // the colour answers NVRMFB gives (RGB, 8 bits per component), which WindowServer asks before compositing
    case kConnectionColorModesSupported: case kConnectionColorMode: *value = 0x00000001; return kIOReturnSuccess;
    case kConnectionColorDepthsSupported: case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported:
        *value = 0x00000002; return kIOReturnSuccess;
    default: return IOFramebuffer::getAttributeForConnection(idx, attr, value);
    }
}

// NVAccel's display pipe asks every head for its scan-out (NVRM_VRAM_FN_SCANOUT).
// Experimental: publish the real physical console framebuffer aperture as a display-pipe resource.
// This is Intel system/display memory, NOT NVIDIA VRAM or a BAR1 offset; the accelerator must
// continue using the CPU copy path and must not attempt NVIDIA hardware access to this address.
IOReturn NMIntelFB::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4)
{
    if (fn && fn->isEqualTo(NVRM_VRAM_FN_SCANOUT)) {
        NVRMVramRequest *r = (NVRMVramRequest *)p1;
        if (!r || r->version != NVRM_VRAM_ABI_VERSION) return kIOReturnBadArgument;
        if (!fSurfMap) mapSurface();
        if (!fSurfMap) return kIOReturnNotReady;

        const uint64_t bytes = static_cast<uint64_t>(fRowBytes) * fHeight;
        const IOPhysicalAddress phys = fApertureBase + fSurf;
        if (!phys || !bytes || !fWidth || !fHeight) return kIOReturnNotReady;

        r->kva = (void *)fSurfMap->getVirtualAddress();
        r->phys = phys;
        r->actualSize = bytes;
        r->size = bytes;
        r->width = fWidth;
        r->height = fHeight;
        r->pitch = fRowBytes;

        // This is called on every presentation, often at 60 Hz or higher.
        // Logging on every scanout query floods the kernel log and can stall
        // WindowServer. Keep one verified aperture breadcrumb per kext load.
        static volatile SInt32 gScanoutABILogged = 0;
        if (OSCompareAndSwap(0, 1, &gScanoutABILogged)) {
            IFBLOG("scanout ABI: real physical address 0x%llx, %ux%u pitch %u, %llu bytes",
                   (unsigned long long)r->phys, r->width, r->height, r->pitch,
                   (unsigned long long)r->actualSize);
        }
        return kIOReturnSuccess;
    }
    return IOFramebuffer::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

#ifdef NMINTELFB_VIRTUAL
// Test loader: attaches a virtual panel under the NVIDIA PCI device on demand, the way the real driver will place the
// Intel panel so WindowServer composites it on the NVIDIA accelerator.
class NMIntelFBLoader : public IOService {
    OSDeclareDefaultStructors(NMIntelFBLoader)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};
OSDefineMetaClassAndStructors(NMIntelFBLoader, IOService)
static int gAttach = 0;
static int nmintelfb_attach_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gAttach, err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr || v != 1 || gAttach == 1) return err;
    // A spare NVRMDisplay nub under NVRM (NVRM's own class), marked as the Intel panel: NMIntelFB claims it ahead of
    // NVRMFB, so the panel sits where NVRMFB does (GPU -> NVRM -> NVRMDisplay -> framebuffer). Starting a framebuffer
    // directly on the NVIDIA PCI device hung the kernel.
    IOService *nub0 = nullptr, *nvrm = nullptr;
    if (OSDictionary *m = IOService::serviceMatching("NVRMDisplay")) {
        if (OSIterator *it = IOService::getMatchingServices(m)) {
            while (OSObject *o = it->getNextObject()) {
                IOService *n = OSDynamicCast(IOService, o);
                OSNumber *fi = n ? OSDynamicCast(OSNumber, n->getProperty("fb-index")) : nullptr;
                if (fi && fi->unsigned32BitValue() == 0) { nub0 = n; nub0->retain(); break; }
            }
            it->release();
        }
        m->release();
    }
    if (nub0) { nvrm = nub0->getProvider(); if (nvrm) nvrm->retain(); }
    if (!nub0 || !nvrm) { OSSafeReleaseNULL(nub0); OSSafeReleaseNULL(nvrm); IFBLOG("attach: no NVRMDisplay 0 / NVRM"); return ENODEV; }
    IOService *nub = OSDynamicCast(IOService, OSMetaClass::allocClassWithName("NVRMDisplay"));
    if (!nub || !nub->init()) { OSSafeReleaseNULL(nub); nub0->release(); nvrm->release(); return ENOMEM; }
    static const char *const kCopy[] = {"nvkms-kapi", "gpu-id", "phys-for-va"};
    for (const char *k : kCopy) if (OSObject *v = nub0->getProperty(k)) nub->setProperty(k, v);
    nub->setProperty("fb-index", 4ull, 32);
    nub->setProperty("nm-intel-panel", kOSBooleanTrue);
    nub0->release();
    if (!nub->attach(nvrm)) { nub->release(); nvrm->release(); return EIO; }
    nvrm->release();
    nub->registerService();
    nub->release();
    gAttach = 1;
    IFBLOG("attach: Intel-panel NVRMDisplay nub published under NVRM");
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nmintelfb_attach, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, nullptr, 0,
            nmintelfb_attach_sysctl, "I", "1 = attach the virtual panel under the NVIDIA display device (test build)");
bool NMIntelFBLoader::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    sysctl_register_oid(&sysctl__debug_nmintelfb_attach);
    IFBLOG("loader idle until debug.nmintelfb_attach=1");
    return true;
}
void NMIntelFBLoader::stop(IOService *provider)
{
    sysctl_unregister_oid(&sysctl__debug_nmintelfb_attach);
    IOService::stop(provider);
}
#endif


