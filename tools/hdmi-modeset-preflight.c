// SPDX-License-Identifier: MIT
// Trigger the root-only, read-only XeGfx DDI B HDMI modeset preflight.
// Compile: xcrun clang -std=c11 -Wall -Wextra -Werror -framework IOKit
//   -framework CoreFoundation tools/hdmi-modeset-preflight.c -o hdmi-modeset-preflight
// Run only after successful HDMI EDID probe: sudo ./hdmi-modeset-preflight
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static void displayProperty(io_service_t svc, CFStringRef key, const char *label) {
    CFTypeRef val=IORegistryEntryCreateCFProperty(svc,key,kCFAllocatorDefault,0);
    if (!val) { printf("%s: unavailable\n",label); return; }
    if (CFGetTypeID(val)==CFStringGetTypeID()) {
        char value[128]={};
        if (CFStringGetCString((CFStringRef)val,value,sizeof(value),kCFStringEncodingUTF8))
            printf("%s: %s\n",label,value);
    } else if (CFGetTypeID(val)==CFNumberGetTypeID()) {
        long long n=0;
        CFNumberGetValue((CFNumberRef)val,kCFNumberLongLongType,&n);
        printf("%s: %lld\n",label,n);
    }
    CFRelease(val);
}
int main(void) {
    io_service_t fb=IOServiceGetMatchingService(kIOMainPortDefault,
                                                IOServiceMatching("NMIntelFB"));
    if(!fb) { fprintf(stderr,"NMIntelFB not found\n"); return 1; }
    const void *keys[]={CFSTR("XeGfxHDMIModesetPreflight")};
    const void *vals[]={kCFBooleanTrue};
    CFDictionaryRef d=CFDictionaryCreate(kCFAllocatorDefault,keys,vals,1,
                        &kCFTypeDictionaryKeyCallBacks,
                        &kCFTypeDictionaryValueCallBacks);
    if(!d){IOObjectRelease(fb);return 1;}
    kern_return_t kr=IORegistryEntrySetCFProperties(fb,d);
    CFRelease(d);
    printf("Read-only HDMI modeset preflight request: 0x%08x\n",(unsigned)kr);
    displayProperty(fb,CFSTR("NMIntelFBHDMIPreflightStatus"),"status");
    displayProperty(fb,CFSTR("NMIntelFBHDMIPreflightPanelFrames"),"eDP frames over 40 ms");
    displayProperty(fb,CFSTR("NMIntelFBHDMIPreflightTargetClockHz"),"HDMI target pixel clock Hz");
    CFTypeRef v=IORegistryEntryCreateCFProperty(
        fb,CFSTR("NMIntelFBHDMIPreflightRegisters"),kCFAllocatorDefault,0);
    if(v && CFGetTypeID(v)==CFDataGetTypeID()) {
        CFDataRef data=(CFDataRef)v;
        const CFIndex n=CFDataGetLength(data);
        if(n<8 || (n%4)!=0) { fprintf(stderr,"Invalid register dump length\n"); kr=1; }
        else {
            const uint8_t *raw=CFDataGetBytePtr(data);
            uint32_t abi=0, count=0;
            memcpy(&abi,raw,4);
            memcpy(&count,raw+4,4);
            if(abi!=0x48445031u || count!=22u || n!=(CFIndex)((2+count*2)*4)) {
                fprintf(stderr,"Unexpected snapshot ABI/count: 0x%08x, %u, %ld bytes\n",
                        abi,count,(long)n);
                kr=1;
            } else {
                printf("Snapshot of %u read-only Intel display MMIO registers:\n",count);
                const char *names[]={
                    "HSW_PWR_WELL_CTL1_BIOS","HSW_PWR_WELL_CTL2_DRV",
                    "ICL_PWR_WELL_CTL_DDI1","ICL_PWR_WELL_CTL_DDI2",
                    "CDCLK_CTL","DDI_CLK_SEL_B","TRANS_CLK_SEL_B",
                    "DDI_CTL_DE_A","DDI_CTL_DE_B","TRANS_DDI_FUNC_CTL_A",
                    "TRANS_DDI_FUNC_CTL_B","TRANSCONF_A","TRANSCONF_B",
                    "PLANE_CTL_A","PLANE_CTL_B","PLANE_SURF_A","PLANE_SURF_B",
                    "HTOTAL_A","HTOTAL_B","HSYNC_B","VTOTAL_B","VSYNC_B"};
                for(uint32_t i=0;i<count;i++) {
                    uint32_t off=0, reg=0;
                    memcpy(&off,raw+8+4*i,4);
                    memcpy(&reg,raw+8+4*count+4*i,4);
                    printf("REG 0x%06x=0x%08x  %s\n",off,reg,names[i]);
                }
            }
        }
    } else {
        printf("No register snapshot. Run a successful DDC EDID probe first.\n");
    }
    if(v)CFRelease(v);
    displayProperty(fb,CFSTR("NMIntelFBHDMIModePlanStatus"),
                    "HDMI mode plan");
    CFTypeRef plan=IORegistryEntryCreateCFProperty(
        fb,CFSTR("NMIntelFBHDMIModePlan"),kCFAllocatorDefault,0);
    if(plan && CFGetTypeID(plan)==CFDataGetTypeID()) {
        const CFDataRef d=(CFDataRef)plan;
        // 10 header words + 7 register offsets + 7 register values.
        if(CFDataGetLength(d)==96) {
            const uint8_t *raw=CFDataGetBytePtr(d);
            uint32_t p[24]={0};
            memcpy(p,raw,sizeof(p));
            if(p[0]==0x484d5031u && p[1]==7u && p[2]==1u &&
               p[3]==1u && p[4]==1u && p[5]==4u) {
                printf("Linux-referenced HDMI target: pipe %u, DDI %u, C10 DPLL%u, %u lanes, %u kHz refclk, %u Hz pixel clock\n",
                       p[2],p[3],p[4],p[5],p[6],p[7]);
                printf("Unimplemented required mode phases bitmask: 0x%08x\n",p[8]);
                printf("Expected refresh: %u mHz\n",p[9]);
                const char *names[]={"HTOTAL_B","HBLANK_B","HSYNC_B",
                                     "VTOTAL_B","VBLANK_B","VSYNC_B","PIPEBSRC"};
                for(unsigned i=0;i<7;i++)
                    printf("TIMING ONLY (NOT WRITTEN) 0x%06x=0x%08x %s\n",
                           p[10+i],p[17+i],names[i]);
            } else {
                fprintf(stderr,"Unexpected HDMI mode plan ABI/header\n");
                kr=1;
            }
        } else {
            fprintf(stderr,"Unexpected HDMI plan byte length: %ld\n",
                    (long)CFDataGetLength(d));
            kr=1;
        }
    } else {
        puts("No validated HDMI timing plan was published.");
    }
    if(plan) CFRelease(plan);
    IOObjectRelease(fb);
    puts("No HDMI clock, PHY, transcoder or pipe was programmed.");
    return kr==KERN_SUCCESS?0:1;
}
