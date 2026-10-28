// XeGfxIntelFramebuffer: Apple's IONDRVFramebuffer under a class name with "Intel" in it. CoreDisplay picks the GPU
// vendor from the IOFramebuffer class name, and IONDRVFramebuffer reads as no vendor, which turns IOPresentment off
// for WindowServer (I-108). Lives in the AuxKC because IONDRVSupport is in the system collection.
// <wtl> fork: nvgfx | author: watchthelight </wtl>
#include <IOKit/ndrvsupport/IONDRVFramebuffer.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <kern/thread_call.h>
#include <kern/clock.h>

#define XIFBLOG(fmt, ...) IOLog("XeGfxIntelFB: " fmt "\n", ##__VA_ARGS__)

// Boot guard as in NMIntelFB: each start counts in NVRAM, 120 s of uptime after start clears it, and two counted starts
// in a row make probe decline so IONDRVFramebuffer drives the panel again. Boot-arg -xeintelfboff declines always.
#define XIFB_GUARD_VAR "xeintelfb-starts"
static unsigned xifbGuardStarts(IORegistryEntry *o)
{
    OSObject *v = o->copyProperty(XIFB_GUARD_VAR); OSData *d = OSDynamicCast(OSData, v);
    unsigned n = (d && d->getLength() >= 1) ? *(const unsigned char *)d->getBytesNoCopy() : 0;
    if (v) v->release();
    return n;
}
static bool xifbGuardTripped()
{
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return false;
    bool t = xifbGuardStarts(o) >= 2; o->release(); return t;
}
static void xifbGuardCount()
{
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return;
    unsigned char n = (unsigned char)(xifbGuardStarts(o) + 1);
    if (OSData *d = OSData::withBytes(&n, 1)) { if (o->setProperty(XIFB_GUARD_VAR, d)) XIFBLOG("boot guard: start %u without WindowServer so far (off after 2)", n); d->release(); }
    o->release();
}
static void xifbGuardClear()
{
    static bool done; if (done) return; done = true;
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return;
    // removeProperty did not reach NVRAM (N-283); a zero count does, the same way the count itself is written
    unsigned char z = 0;
    if (OSData *d = OSData::withBytes(&z, 1)) { if (o->setProperty(XIFB_GUARD_VAR, d)) XIFBLOG("boot guard cleared: still up 120 s after start"); d->release(); }
    o->release();
}

// kmutil places a kext in a kernel collection only with a _kmod_info symbol (Xcode generates it; this build does not)
#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(org.watchthelight.XeGfxIntelFB, "0.6", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

class XeGfxIntelFramebuffer : public IONDRVFramebuffer {
    OSDeclareDefaultStructors(XeGfxIntelFramebuffer)
public:
    IOService *probe(IOService *provider, SInt32 *score) override {
        int off = 0;
        if (PE_parse_boot_argn("-xeintelfboff", &off, sizeof off)) { XIFBLOG("off by boot-arg"); return nullptr; }
        if (xifbGuardTripped()) { XIFBLOG("boot guard tripped (two starts without WindowServer): declining"); return nullptr; }
        return IONDRVFramebuffer::probe(provider, score);
    }
    bool start(IOService *provider) override {
        xifbGuardCount();
        // System Profiler names the GPU from the PCI device's "model"; without it About This Mac shows a bare "Display"
        static const char model[] = "Intel Xe Graphics";
        if (!provider->getProperty("model"))
            if (OSData *m = OSData::withBytes(model, sizeof model)) { provider->setProperty("model", m); m->release(); }
        // System Profiler shows VRAM,totalMB of an Intel PCI device as "VRAM (Dynamic, Max)", else the 15 MB GOP
        // framebuffer. Same size ANV gives its heap: 3/4 of RAM above 4 GB, half below. xeintelfbvram=<MB> overrides.
        if (!provider->getProperty("VRAM,totalMB")) {
            uint32_t mb = 0;
            if (!PE_parse_boot_argn("xeintelfbvram", &mb, sizeof mb)) {
                uint64_t ram = 0; size_t len = sizeof ram;
                if (!sysctlbyname("hw.memsize", &ram, &len, nullptr, 0) && ram)
                    mb = (uint32_t)((ram <= (4ull << 30) ? ram / 2 : ram / 4 * 3) >> 20);
            }
            if (mb) { provider->setProperty("VRAM,totalMB", (unsigned long long)mb, 32); XIFBLOG("VRAM,totalMB %u", mb); }
        }
        // the guard is for boots that hang: still running 120 s after start clears it. IOGraphics keeps the
        // WindowServer-active attribute and WindowServer's connection skips newUserClient here (N-282 to N-284)
        static thread_call_t tc;
        if (!tc) tc = thread_call_allocate([](thread_call_param_t, thread_call_param_t) { xifbGuardClear(); }, nullptr);
        if (tc) { uint64_t dl; clock_interval_to_deadline(120, kSecondScale, &dl); thread_call_enter_delayed(tc, dl); }
        return IONDRVFramebuffer::start(provider);
    }
};

OSDefineMetaClassAndStructors(XeGfxIntelFramebuffer, IONDRVFramebuffer)
