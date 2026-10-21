// IGPUConsoleRange: lets Apple's IONDRVFramebuffer drive a panel the firmware lit on an Intel iGPU whose boot
// framebuffer lives in stolen memory (Meteor/Arrow Lake: GOP framebuffer = DSM base, outside every PCI BAR).
// IOBootNDRV::fromRegistryEntry only accepts a console that lies inside one of the device's memory ranges, so on these
// parts it never builds its VRAM descriptor and WindowServer's open fails (kIOReturnUnsupported). This driver probes
// first in the IOFramebuffer category, appends the console range to the iGPU's device memory, and declines, so
// IONDRVFramebuffer probes next and finds it. It acts only when the console is the iGPU's own stolen memory.
// <wtl> fork: nvgfx | author: watchthelight </wtl>

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSArray.h>
#include <mach/kmod.h>

#define RLOG(fmt, ...) IOLog("IGPUConsoleRange: " fmt "\n", ##__VA_ARGS__)

extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(org.watchthelight.IGPUConsoleRange, "0.2", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *, void *) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *, void *) { return KERN_SUCCESS; }

class IGPUConsoleRange : public IOService {
    OSDeclareDefaultStructors(IGPUConsoleRange)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        RLOG("holding the iGPU's framebuffer slot: the NVIDIA proxy head (nvfbigpu=1) drives this panel, not IONDRV");
        return true;
    }
};
OSDefineMetaClassAndStructors(IGPUConsoleRange, IOService)

IOService *IGPUConsoleRange::probe(IOService *provider, SInt32 *score)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci || pci->getFunctionNumber() != 0) return nullptr;
    char b[4];
    if (PE_parse_boot_argn("-igpurangeoff", b, sizeof b)) { RLOG("off by boot-arg"); return nullptr; }

    PE_Video v; bzero(&v, sizeof v);
    if (!getPlatform() || getPlatform()->getConsoleInfo(&v) != kIOReturnSuccess || !v.v_baseAddr || v.v_depth != 32) return nullptr;
    const uint64_t base = (uint64_t)v.v_baseAddr & ~3ull, bytes = (uint64_t)v.v_rowBytes * v.v_height;
    if (!bytes) return nullptr;

    for (IOItemCount i = 0; i < pci->getDeviceMemoryCount(); i++)
        if (IODeviceMemory *m = pci->getDeviceMemoryWithIndex(i)) {
            const uint64_t pa = m->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
            if (base >= pa && base < pa + m->getLength()) { RLOG("console 0x%llx already in range %u", base, (unsigned)i); return nullptr; }
        }

    // DSM base from MMIO (config 0xC0 reads 0 on display 14); BAR0 is mapped only for this one read
    if (!(pci->configRead16(kIOPCIConfigCommand) & kIOPCICommandMemorySpace)) { RLOG("memory decode off, not touching it"); return nullptr; }
    IOMemoryMap *mm = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!mm || mm->getLength() < 0x108108) { OSSafeReleaseNULL(mm); return nullptr; }
    volatile uint32_t *r = (volatile uint32_t *)mm->getVirtualAddress();
    const uint64_t dsm = (((uint64_t)r[0x1080C4 / 4] << 32) | r[0x1080C0 / 4]) & ~0xFFFFFull;
    mm->release();
    if (!dsm || base < dsm || base - dsm >= 0x10000000ull) {
        RLOG("console 0x%llx is not this GPU's stolen memory (DSM 0x%llx), leaving the device alone", base, dsm);
        return nullptr;
    }

    OSArray *old = pci->getDeviceMemory();
    OSArray *arr = old ? OSArray::withArray(old, old->getCount() + 1) : OSArray::withCapacity(1);
    IODeviceMemory *dm = IODeviceMemory::withRange((IOPhysicalAddress)base, (IOPhysicalLength)round_page_64(bytes));
    if (!arr || !dm) { OSSafeReleaseNULL(arr); OSSafeReleaseNULL(dm); return nullptr; }
    arr->setObject(dm); dm->release();
    pci->setDeviceMemory(arr); arr->release();
    pci->setProperty("watchthelight-console-range", (unsigned long long)base, 64);
    RLOG("console 0x%llx (%lux%lu rowBytes %lu) is DSM+0x%llx: added as device memory %u for IONDRVFramebuffer",
         base, v.v_width, v.v_height, v.v_rowBytes, base - dsm, (unsigned)pci->getDeviceMemoryCount() - 1);
    // nvfbigpu=1: NVRMFB's proxy head drives this panel, so IONDRV must not start a second framebuffer on the same
    // memory; holding the category blocks it. Otherwise decline and let IONDRVFramebuffer probe next.
    int proxy = 0;
    if (PE_parse_boot_argn("nvfbigpu", &proxy, sizeof proxy) && proxy) {
        bool nv = false;   // only with an NVIDIA GPU on PCI to stand in; otherwise IONDRV keeps the panel (software desktop)
        if (OSDictionary *m = IOService::serviceMatching("IOPCIDevice")) {
            if (OSIterator *it = IOService::getMatchingServices(m)) {
                while (OSObject *o = it->getNextObject())
                    if (IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o))
                        if (d->configRead16(kIOPCIConfigVendorID) == 0x10de && (d->configRead8(kIOPCIConfigClassCode + 2) == 0x03)) { nv = true; break; }
                it->release();
            }
            m->release();
        }
        if (nv) return this;
        RLOG("nvfbigpu=1 but no NVIDIA GPU on PCI: IONDRVFramebuffer keeps the panel");
    }
    return nullptr;
}
