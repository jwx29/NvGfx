// SPDX-License-Identifier: MIT
// Root-only diagnostic for an independently pinned i915 GGTT HDMI surface.
// This does NOT configure HDMI hardware, an IOFramebuffer or WindowServer.
// Build:
// xcrun clang -std=c11 -Wall -Wextra -Werror -framework IOKit
//   -framework CoreFoundation tools/hdmi-ggtt-surface.c -o hdmi-ggtt-surface
// Usage: sudo ./hdmi-ggtt-surface allocate|query|release
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static void show(io_service_t s, CFStringRef k, const char *label) {
    CFTypeRef v=IORegistryEntryCreateCFProperty(s,k,kCFAllocatorDefault,0);
    if (!v) {printf("%s: unavailable\n",label);return;}
    if (CFGetTypeID(v)==CFStringGetTypeID()) {
        char b[180]={0};
        if(CFStringGetCString((CFStringRef)v,b,sizeof b,kCFStringEncodingUTF8))
            printf("%s: %s\n",label,b);
    } else if (CFGetTypeID(v)==CFNumberGetTypeID()) {
        long long n=0;
        CFNumberGetValue((CFNumberRef)v,kCFNumberLongLongType,&n);
        printf("%s: %lld (0x%llx)\n",label,n,(unsigned long long)n);
    }
    CFRelease(v);
}
int main(int argc, char **argv) {
    if (argc!=2) {
        fprintf(stderr,"Usage: %s allocate|query|release\n",argv[0]);
        return 2;
    }
    CFStringRef key=NULL;
    if (!strcmp(argv[1],"allocate"))
        key=CFSTR("XeGfxHDMIScanoutAllocate");
    else if(!strcmp(argv[1],"query"))
        key=CFSTR("XeGfxHDMIScanoutQuery");
    else if(!strcmp(argv[1],"release"))
        key=CFSTR("XeGfxHDMIScanoutRelease");
    else {fprintf(stderr,"Unknown operation: %s\n",argv[1]);return 2;}
    io_service_t resident=IOServiceGetMatchingService(
        kIOMainPortDefault,IOServiceMatching("XeGfxResident"));
    if (!resident) {
        fprintf(stderr,"XeGfxResident unavailable: XeGfx stage 4 must already be active\n");
        return 1;
    }
    const void *keys[]={key},*vals[]={kCFBooleanTrue};
    CFDictionaryRef props=CFDictionaryCreate(kCFAllocatorDefault,keys,vals,1,
                   &kCFTypeDictionaryKeyCallBacks,
                   &kCFTypeDictionaryValueCallBacks);
    if (!props){IOObjectRelease(resident);return 1;}
    kern_return_t kr=IORegistryEntrySetCFProperties(resident,props);
    CFRelease(props);
    printf("XeGfx HDMI GGTT %s result: 0x%08x\n",argv[1],(unsigned)kr);
    show(resident,CFSTR("XeGfxHDMIScanoutStatus"),"State");
    show(resident,CFSTR("XeGfxHDMIScanoutLastErrno"),"Linux errno magnitude");
    show(resident,CFSTR("XeGfxHDMIScanoutGGTTOffset"),"GPU GGTT offset");
    show(resident,CFSTR("XeGfxHDMIScanoutBytes"),"Bytes");
    show(resident,CFSTR("XeGfxHDMIScanoutPitch"),"Pitch");
    show(resident,CFSTR("XeGfxHDMIScanoutPlanStatus"),"Plane B plan");
    show(resident,CFSTR("XeGfxHDMIScanoutEDPGGTTOffset"),"eDP GGTT base");
    show(resident,CFSTR("XeGfxHDMIScanoutEDPBytes"),"eDP scanout bytes");

    // Render the kernel-generated register image, but never write any MMIO.
    CFTypeRef plan=IORegistryEntryCreateCFProperty(
        resident,CFSTR("XeGfxHDMIScanoutPlaneImage"),kCFAllocatorDefault,0);
    if (plan && CFGetTypeID(plan)==CFDataGetTypeID()) {
        const CFDataRef data=(CFDataRef)plan;
        // Four header words, six offsets, six values: 16 uint32_t words.
        if (CFDataGetLength(data)==(CFIndex)(16u*sizeof(uint32_t))) {
            uint32_t words[16]={0};
            memcpy(words,CFDataGetBytePtr(data),sizeof(words));
            if (words[0]==0x48504231u && words[1]==6u &&
                words[2]==5u && words[3]==0u) {
                const char *names[]={
                    "PLANE_STRIDE_1_B","PLANE_POS_1_B","PLANE_SIZE_1_B",
                    "PLANE_OFFSET_1_B","PLANE_CTL_1_B","PLANE_SURF_1_B"
                };
                puts("HDMI plane B MMIO image (NOT WRITTEN):");
                for (unsigned i=0;i<6;++i)
                    printf("  0x%06x = 0x%08x %s%s\n",words[4+i],
                           words[10+i],names[i],
                           i==5 ? " (arm last; NOT WRITTEN)" : "");
            } else {
                fputs("Invalid HDMI plane image header\n",stderr);
            }
        } else {
            fputs("Invalid HDMI plane image size\n",stderr);
        }
    }
    if (plan) CFRelease(plan);
    IOObjectRelease(resident);
    return kr==KERN_SUCCESS?0:1;
}
