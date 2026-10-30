// SPDX-License-Identifier: MIT
// Portable standalone DTD test: c++ -std=c++17 -Wall -Wextra -Werror
// -I kexts/NMIntelFB tools/test_hdmi_edid_timing.cpp -o /tmp/test_hdmi_edid_timing
#include "hdmi_edid_timing.h"
#include <cassert>
#include <cstdio>
#include <cstring>
static void fixChecksum(uint8_t *e) {
    unsigned s=0; for (unsigned i=0;i<127;++i) s+=e[i];
    e[127]=uint8_t(0u-(s&255u));
}
int main(int argc, char **argv) {
    // Synthetic HP 25x 1080p60 preferred DTD; no monitor serial included.
    uint8_t e[128]={};const uint8_t sig[8]={0,255,255,255,255,255,255,0};
    std::memcpy(e,sig,8);
    const uint8_t dtd[18]={
        0x02,0x3a,0x80,0x18,0x71,0x38,0x2d,0x40,0x58,
        0x2c,0x45,0x00,0x20,0x2f,0x21,0x00,0x00,0x1e
    };
    std::memcpy(e+54,dtd,sizeof(dtd));fixChecksum(e);
    nm_hdmi::Timing t={};
    assert(nm_hdmi::parsePreferredTiming(e,sizeof(e),t));
    assert(t.pixelClockHz==148500000u);
    assert(t.hActive==1920 && t.hSyncStart==2008 &&
           t.hSyncEnd==2052 && t.hTotal==2200);
    assert(t.vActive==1080 && t.vSyncStart==1084 &&
           t.vSyncEnd==1089 && t.vTotal==1125);
    assert(t.edidSyncFlags==0x1e);
    e[55]^=1;assert(!nm_hdmi::parsePreferredTiming(e,sizeof(e),t));
    e[55]^=1;
    assert(!nm_hdmi::parsePreferredTiming(e,127,t));
    e[71]=0;fixChecksum(e);
    assert(!nm_hdmi::parsePreferredTiming(e,sizeof(e),t));
    if (argc==2) {
        FILE *f=std::fopen(argv[1],"rb");
        assert(f);
        uint8_t real[256]={};size_t n=std::fread(real,1,sizeof(real),f);
        std::fclose(f);
        assert(n>=128 && nm_hdmi::parsePreferredTiming(real,n,t));
        assert(t.pixelClockHz==148500000u && t.hActive==1920 &&
               t.vActive==1080 && t.hTotal==2200 && t.vTotal==1125);
    }
    std::puts("PASS: 1080p60 DTD clock/sync/blanking; rejects short, checksum and sync-invalid EDID");
}
