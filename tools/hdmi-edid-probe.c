// SPDX-License-Identifier: MIT
// One-shot, root-authorized diagnostic trigger for NMIntelFB's GMBUS EDID read.
// Build: xcrun clang -Wall -Wextra -framework IOKit -framework CoreFoundation
//        -o hdmi-edid-probe tools/hdmi-edid-probe.c
// Usage: sudo ./hdmi-edid-probe [optional-output-path]
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void show(CFTypeRef v, const char *key) {
    if (!v) { printf("%s: not published\n",key); return; }
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        char s[256]={0}; CFStringGetCString((CFStringRef)v,s,sizeof(s),kCFStringEncodingUTF8);
        printf("%s: %s\n",key,s);
    } else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        long long n=0; CFNumberGetValue((CFNumberRef)v,kCFNumberLongLongType,&n);
        printf("%s: %lld\n",key,n);
    }
}
int main(int argc, char **argv) {
    if (argc>2) { fprintf(stderr,"usage: %s [output-edid.bin]\n",argv[0]); return 2; }
    CFMutableDictionaryRef match=IOServiceMatching("NMIntelFB");
    if (!match) return 1;
    io_service_t fb=IOServiceGetMatchingService(kIOMainPortDefault,match);
    if (!fb) { fprintf(stderr,"NMIntelFB service not found\n"); return 1; }
    const void *keys[]={CFSTR("XeGfxHDMIEDIDProbe")};
    const void *vals[]={kCFBooleanTrue};
    CFDictionaryRef request=CFDictionaryCreate(kCFAllocatorDefault,keys,vals,1,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if (!request) { IOObjectRelease(fb); return 1; }
    kern_return_t kr=IORegistryEntrySetCFProperties(fb,request);
    CFRelease(request);
    printf("Probe request: 0x%x%s\n",(unsigned)kr,kr==KERN_SUCCESS?" (accepted)":" (failed)");
    const char *names[]={"NMIntelFBHDMIEDIDStatus","NMIntelFBHDMIEDIDLength",
                         "NMIntelFBHDMIEDIDGMBUS2"};
    CFStringRef cfnames[]={CFSTR("NMIntelFBHDMIEDIDStatus"),
                           CFSTR("NMIntelFBHDMIEDIDLength"),
                           CFSTR("NMIntelFBHDMIEDIDGMBUS2")};
    for (unsigned i=0;i<3;++i) {
        CFTypeRef v=IORegistryEntryCreateCFProperty(fb,cfnames[i],kCFAllocatorDefault,0);
        show(v,names[i]); if(v)CFRelease(v);
    }
    CFTypeRef data=IORegistryEntryCreateCFProperty(fb,CFSTR("NMIntelFBHDMIEDID"),kCFAllocatorDefault,0);
    if (data && CFGetTypeID(data)==CFDataGetTypeID()) {
        CFDataRef d=(CFDataRef)data;
        printf("Validated EDID: %ld bytes\n",(long)CFDataGetLength(d));
        const char *name=argc==2?argv[1]:"hdmi-edid-macos.bin";
        FILE *f=fopen(name,"wb");
        if (!f || fwrite(CFDataGetBytePtr(d),1,(size_t)CFDataGetLength(d),f)!=(size_t)CFDataGetLength(d)) {
            fprintf(stderr,"Could not write %s\n",name);
            if(f) fclose(f);
            CFRelease(data); IOObjectRelease(fb); return 1;
        }
        fclose(f); printf("EDID saved to %s\n",name);
    }
    if(data)CFRelease(data);
    IOObjectRelease(fb);
    return kr==KERN_SUCCESS?0:1;
}
