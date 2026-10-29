// SPDX-License-Identifier: MIT
// Report native HDMI backend readiness. This is a DRY RUN:
// never writes DDI/PLL/PHY/GGTT/display registers.
// Requires a successful hdmi-edid-probe and hdmi-modeset-preflight
// on the same boot. The native backend is not yet implemented.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
static void show(io_service_t fb, CFStringRef key, const char *label) {
    CFTypeRef value=IORegistryEntryCreateCFProperty(fb,key,kCFAllocatorDefault,0);
    if (!value) { printf("%s: (not published)\n",label); return; }
    if (CFGetTypeID(value)==CFStringGetTypeID()) {
        char text[128]={0};
        if (CFStringGetCString((CFStringRef)value,text,sizeof(text),
                               kCFStringEncodingUTF8))
            printf("%s: %s\n",label,text);
    } else if (CFGetTypeID(value)==CFNumberGetTypeID()) {
        long long n=0;
        CFNumberGetValue((CFNumberRef)value,kCFNumberLongLongType,&n);
        printf("%s: %lld\n",label,n);
    }
    CFRelease(value);
}
int main(void) {
    io_service_t fb=IOServiceGetMatchingService(
        kIOMainPortDefault,IOServiceMatching("NMIntelFB"));
    if (!fb) { fprintf(stderr,"NMIntelFB not found\n"); return 1; }
    const void *keys[]={CFSTR("XeGfxHDMINativeDryRun")};
    const void *values[]={kCFBooleanTrue};
    CFDictionaryRef request=CFDictionaryCreate(
        kCFAllocatorDefault,keys,values,1,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    if(!request){IOObjectRelease(fb);return 1;}
    const kern_return_t ret=IORegistryEntrySetCFProperties(fb,request);
    CFRelease(request);
    printf("Dry-run request returned 0x%08x (unsupported is EXPECTED until the backend exists)\n",
           (unsigned)ret);
    show(fb,CFSTR("NMIntelFBHDMINativeStatus"),"Native HDMI state");
    show(fb,CFSTR("NMIntelFBHDMINativeMissingStage"),"First missing backend stage");
    show(fb,CFSTR("NMIntelFBHDMINativeStagesApplied"),"Hardware stages applied");
    show(fb,CFSTR("NMIntelFBHDMIPowerWellStatus"),"PW_2/PW_B power");
    show(fb,CFSTR("NMIntelFBHDMIDCStateEnable"),"DC_STATE_EN raw");
    show(fb,CFSTR("NMIntelFBHDMIDCPolicyStatus"),"DC state policy");
    puts("This diagnostic has not enabled an HDMI display.");
    IOObjectRelease(fb);
    return 0;
}
