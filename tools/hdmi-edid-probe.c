// SPDX-License-Identifier: MIT
// Explicit root-authorized HDMI DDC/EDID probe; never enables HDMI video.
// Build: xcrun clang -Wall -Wextra -Werror -framework IOKit
//        -framework CoreFoundation tools/hdmi-edid-probe.c -o hdmi-edid-probe
// Usage:
//   sudo ./hdmi-edid-probe [output-edid.bin]
//   sudo ./hdmi-edid-probe --watch [interval-seconds] (Ctrl-C to stop)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void show(CFTypeRef v, const char *key) {
    if (!v) { printf("%s: not published\n", key); return; }
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        char s[256] = {0};
        if (CFStringGetCString((CFStringRef)v, s, sizeof(s), kCFStringEncodingUTF8))
            printf("%s: %s\n", key, s);
    } else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        long long n = 0;
        CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
        printf("%s: %lld\n", key, n);
    }
}
static void printProperty(io_service_t fb, CFStringRef key, const char *name) {
    CFTypeRef v=IORegistryEntryCreateCFProperty(fb, key, kCFAllocatorDefault, 0);
    show(v, name);
    if (v) CFRelease(v);
}
static int probe(io_service_t fb, CFDictionaryRef request, const char *output) {
    kern_return_t kr=IORegistryEntrySetCFProperties(fb, request);
    printf("Probe request: 0x%x%s\n", (unsigned)kr,
           kr == KERN_SUCCESS ? " (accepted)" : " (failed)");
    printProperty(fb, CFSTR("NMIntelFBHDMIEDIDStatus"), "NMIntelFBHDMIEDIDStatus");
    printProperty(fb, CFSTR("NMIntelFBHDMIEDIDLength"), "NMIntelFBHDMIEDIDLength");
    printProperty(fb, CFSTR("NMIntelFBHDMIEDIDGMBUS2"), "NMIntelFBHDMIEDIDGMBUS2");
    printProperty(fb, CFSTR("NMIntelFBHDMIDDCState"), "NMIntelFBHDMIDDCState");
    printProperty(fb, CFSTR("NMIntelFBHDMIDDCGeneration"), "NMIntelFBHDMIDDCGeneration");
    printProperty(fb, CFSTR("NMIntelFBHDMIDDCProbeCount"), "NMIntelFBHDMIDDCProbeCount");
    CFTypeRef data=IORegistryEntryCreateCFProperty(
        fb, CFSTR("NMIntelFBHDMIEDID"), kCFAllocatorDefault, 0);
    int rc=kr == KERN_SUCCESS ? 0 : 1;
    if (data && CFGetTypeID(data) == CFDataGetTypeID()) {
        CFDataRef d=(CFDataRef)data;
        printf("Validated EDID: %ld bytes\n", (long)CFDataGetLength(d));
        if (output) {
            FILE *f=fopen(output, "wb");
            if (!f || fwrite(CFDataGetBytePtr(d),1,
                    (size_t)CFDataGetLength(d),f) !=
                    (size_t)CFDataGetLength(d)) {
                fprintf(stderr,"Could not write %s\n",output);
                rc=1;
            } else {
                printf("EDID saved to %s\n",output);
            }
            if (f) fclose(f);
        }
    }
    if (data) CFRelease(data);
    fflush(stdout);
    return rc;
}
int main(int argc, char **argv) {
    const int watch=argc>=2 && strcmp(argv[1], "--watch")==0;
    unsigned interval=5;
    const char *output="hdmi-edid-macos.bin";
    if (watch) {
        output=NULL; // never overwrite/swap the known-good EDID when watching
        if (argc>3) { fprintf(stderr,"usage: %s --watch [2..60 seconds]\n",argv[0]); return 2; }
        if (argc==3) {
            char *end=NULL;
            long v=strtol(argv[2],&end,10);
            if (!end || *end || v<2 || v>60) {
                fprintf(stderr,"Polling interval must be 2..60 seconds\n");
                return 2;
            }
            interval=(unsigned)v;
        }
    } else if (argc>2) {
        fprintf(stderr,"usage: %s [output-edid.bin]\n",argv[0]);
        return 2;
    } else if (argc==2) {
        output=argv[1];
    }
    CFMutableDictionaryRef match=IOServiceMatching("NMIntelFB");
    if (!match) return 1;
    io_service_t fb=IOServiceGetMatchingService(kIOMainPortDefault,match);
    if (!fb) { fprintf(stderr,"NMIntelFB service not found\n"); return 1; }
    const void *keys[]={CFSTR("XeGfxHDMIEDIDProbe")};
    const void *vals[]={kCFBooleanTrue};
    CFDictionaryRef request=CFDictionaryCreate(kCFAllocatorDefault,keys,vals,1,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if (!request) { IOObjectRelease(fb); return 1; }
    int rc=0;
    if (watch) {
        printf("Polling HDMI DDC every %u seconds; Ctrl-C to stop. This does not enable HDMI video.\n",interval);
        for (;;) {
            (void)probe(fb,request,NULL);
            puts("---");
            sleep(interval);
        }
    } else {
        rc=probe(fb,request,output);
    }
    CFRelease(request);
    IOObjectRelease(fb);
    return rc;
}
