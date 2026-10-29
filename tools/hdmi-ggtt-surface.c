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
    IOObjectRelease(resident);
    return kr==KERN_SUCCESS?0:1;
}
