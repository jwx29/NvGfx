/* XeGfx reporting-only helper. Adapted to NMIntelFB and Intel PCI 8086:7d55.
 * Reads Vulkan physical-device heap capacity; never allocates GPU memory. */
#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>
#include <dlfcn.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#define INTEL_ICD "/Library/GPUBundles/nvmtl/libvulkan_intel.dylib"

static uint16_t pciID(io_registry_entry_t pci, CFStringRef key)
{
    CFTypeRef value = IORegistryEntryCreateCFProperty(pci, key, kCFAllocatorDefault, 0);
    uint16_t id = 0;
    if (value && CFGetTypeID(value) == CFDataGetTypeID() &&
        CFDataGetLength((CFDataRef)value) >= 2) {
        const UInt8 *p = CFDataGetBytePtr((CFDataRef)value);
        id = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    }
    if (value) CFRelease(value);
    return id;
}

static NSDictionary *queryANV(void)
{
    uint64_t ram = 0;
    size_t len = sizeof(ram);
    if (sysctlbyname("hw.memsize", &ram, &len, NULL, 0) ||
        len != sizeof(ram) || !ram) {
        fprintf(stderr, "Could not query physical RAM\n");
        return nil;
    }

    void *lib = dlopen(INTEL_ICD, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "Could not open Intel ICD: %s\n", dlerror());
        return nil;
    }
    NSDictionary *report = nil;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr get =
        (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!get) get = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");

    do {
        if (!get) break;
        PFN_vkCreateInstance create =
            (PFN_vkCreateInstance)get(VK_NULL_HANDLE, "vkCreateInstance");
        if (!create) break;
        VkApplicationInfo app = {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "XeGfxGPUReport",
            .apiVersion = VK_API_VERSION_1_1
        };
        VkInstanceCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app
        };
        if (create(&ci, NULL, &instance) != VK_SUCCESS) break;

        PFN_vkEnumeratePhysicalDevices enumerate =
            (PFN_vkEnumeratePhysicalDevices)get(instance, "vkEnumeratePhysicalDevices");
        PFN_vkGetPhysicalDeviceProperties properties =
            (PFN_vkGetPhysicalDeviceProperties)get(instance, "vkGetPhysicalDeviceProperties");
        PFN_vkGetPhysicalDeviceMemoryProperties memory =
            (PFN_vkGetPhysicalDeviceMemoryProperties)get(instance, "vkGetPhysicalDeviceMemoryProperties");
        if (!enumerate || !properties || !memory) break;

        uint32_t count = 0;
        if (enumerate(instance, &count, NULL) != VK_SUCCESS || count == 0 || count > 16)
            break;
        VkPhysicalDevice devices[16] = {0};
        if (enumerate(instance, &count, devices) != VK_SUCCESS) break;
        VkPhysicalDevice selected = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties selectedProps = {0};
        unsigned matches = 0;
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties p = {0};
            properties(devices[i], &p);
            if (p.vendorID == 0x8086 && p.deviceID == 0x7d55 &&
                p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
                selected = devices[i]; selectedProps = p; ++matches;
            }
        }
        if (matches != 1) break;
        VkPhysicalDeviceMemoryProperties heaps = {0};
        memory(selected, &heaps);
        if (!heaps.memoryHeapCount || heaps.memoryHeapCount > VK_MAX_MEMORY_HEAPS ||
            !heaps.memoryTypeCount || heaps.memoryTypeCount > VK_MAX_MEMORY_TYPES)
            break;
        BOOL valid = YES;
        for (uint32_t i = 0; i < heaps.memoryTypeCount; ++i)
            if (heaps.memoryTypes[i].heapIndex >= heaps.memoryHeapCount) valid = NO;
        uint64_t capacity = 0;
        for (uint32_t i = 0; i < heaps.memoryHeapCount; ++i) {
            VkMemoryHeap h = heaps.memoryHeaps[i];
            if (!h.size || h.size > ram) valid = NO;
            if ((h.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && h.size > capacity)
                capacity = h.size;
        }
        if (!valid || capacity < (256ull << 20) ||
            capacity > ram || (capacity >> 20) > UINT32_MAX)
            break;
        size_t nameLen = strnlen(selectedProps.deviceName, sizeof(selectedProps.deviceName));
        if (!nameLen || nameLen > 127 || nameLen >= sizeof(selectedProps.deviceName))
            break;
        BOOL nameValid = YES;
        for (size_t i = 0; i < nameLen; ++i)
            if ((unsigned char)selectedProps.deviceName[i] < 0x20 ||
                (unsigned char)selectedProps.deviceName[i] > 0x7e) nameValid = NO;
        if (!nameValid) break;
        report = @{
            @"version": @1, @"vendor": @(selectedProps.vendorID),
            @"device": @(selectedProps.deviceID),
            @"name": [NSString stringWithUTF8String:selectedProps.deviceName],
            @"sharedHeapBytes": @(capacity), @"physicalRAMBytes": @(ram)
        };
    } while (0);

    if (instance != VK_NULL_HANDLE && get) {
        PFN_vkDestroyInstance destroy =
            (PFN_vkDestroyInstance)get(instance, "vkDestroyInstance");
        if (destroy) destroy(instance, NULL);
    }
    dlclose(lib);
    if (!report) fprintf(stderr, "No valid Intel 8086:7d55 Vulkan shared heap found\n");
    return report;
}

static int publish(NSDictionary *report)
{
    io_iterator_t iter = IO_OBJECT_NULL;
    kern_return_t rc = IOServiceGetMatchingServices(kIOMainPortDefault,
                                                     IOServiceMatching("NMIntelFB"), &iter);
    if (rc != KERN_SUCCESS) return 3;
    io_service_t found = IO_OBJECT_NULL;
    io_service_t candidate = IO_OBJECT_NULL;
    unsigned matches = 0;
    while ((candidate = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        io_registry_entry_t pci = IO_OBJECT_NULL;
        if (IORegistryEntryGetParentEntry(candidate, kIOServicePlane, &pci) == KERN_SUCCESS) {
            if (pciID(pci, CFSTR("vendor-id")) == 0x8086 &&
                pciID(pci, CFSTR("device-id")) == 0x7d55) {
                ++matches;
                if (!found) { found = candidate; candidate = IO_OBJECT_NULL; }
            }
            IOObjectRelease(pci);
        }
        if (candidate) IOObjectRelease(candidate);
    }
    IOObjectRelease(iter);
    if (matches != 1 || !found) {
        if (found) IOObjectRelease(found);
        fprintf(stderr, "Expected one NMIntelFB on Intel 8086:7d55, got %u\n", matches);
        return 3;
    }

    rc = IORegistryEntrySetCFProperties(found,
        (__bridge CFTypeRef)@{ @"XeGfxGPUIdentity": report });
    io_registry_entry_t pci = IO_OBJECT_NULL;
    BOOL verified = NO;
    if (rc == KERN_SUCCESS &&
        IORegistryEntryGetParentEntry(found, kIOServicePlane, &pci) == KERN_SUCCESS) {
        CFTypeRef val = IORegistryEntryCreateCFProperty(pci,
            CFSTR("VRAM,totalMB"), kCFAllocatorDefault, 0);
        verified = val && CFGetTypeID(val) == CFNumberGetTypeID() &&
            [(__bridge NSNumber *)val unsignedLongLongValue] ==
                ([report[@"sharedHeapBytes"] unsignedLongLongValue] >> 20);
        if (val) CFRelease(val);
    }
    if (pci) IOObjectRelease(pci);
    IOObjectRelease(found);
    if (!verified) {
        fprintf(stderr, "PCI publication/readback FAILED, kernel status 0x%x\n", rc);
        return 4;
    }
    puts("Verified VRAM,totalMB on the physical Intel PCI device");
    return 0;
}

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        BOOL queryOnly = argc == 2 && !strcmp(argv[1], "--query-only");
        if (argc > 2 || (argc == 2 && !queryOnly)) {
            fprintf(stderr, "Usage: xegfx-gpu-report [--query-only]\n");
            return 2;
        }
        if (!queryOnly && geteuid() != 0) {
            fprintf(stderr, "Publishing requires sudo\n");
            return 2;
        }
        NSDictionary *report = queryANV();
        if (!report) return 2;
        NSData *json = [NSJSONSerialization dataWithJSONObject:report
                        options:NSJSONWritingPrettyPrinted error:NULL];
        fwrite(json.bytes, 1, json.length, stdout);
        putchar('\n');
        return queryOnly ? 0 : publish(report);
    }
}
