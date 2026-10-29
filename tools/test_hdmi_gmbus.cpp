// SPDX-License-Identifier: MIT
// Portable fixture for the bounded Meteor Lake GMBUS state machine.
// Run: c++ -std=c++17 -Wall -Wextra -Werror -I kexts/NMIntelFB
//      tools/test_hdmi_gmbus.cpp -o /tmp/test_hdmi_gmbus && /tmp/test_hdmi_gmbus
#include "hdmi_gmbus.h"
#include <cassert>
#include <cstring>
#include <cstdio>
struct Fake {
    uint8_t edid[256]={};
    unsigned pos=0, words=0, delays=0, resets=0, starts=0, stops=0;
    uint32_t pin=0;
    bool missing=false, busy=false, stalled=false, inaccessible=false;
    uint32_t read32(uint32_t addr) {
        if (addr==nm_hdmi::kGMBUS0) return pin;
        if (addr==nm_hdmi::kGMBUS2) {
            if (inaccessible) return 0xffffffffu;
            if (stalled) return 0;
            if (busy && !starts) return nm_hdmi::kInUse;
            if (missing) return nm_hdmi::kNAK;
            return nm_hdmi::kReady | nm_hdmi::kWaitPhase;
        }
        if (addr==nm_hdmi::kGMBUS3) {
            uint32_t v=0;
            for (unsigned i=0;i<4;i++) v|=uint32_t(edid[pos++])<<(8*i);
            ++words; return v;
        }
        return 0;
    }
    void write32(uint32_t addr,uint32_t v) {
        if(addr==nm_hdmi::kGMBUS0) pin=v;
        if(addr==nm_hdmi::kGMBUS1) {
            if(v & nm_hdmi::kCycleIndex) { ++starts; pos=(v>>8)&255; }
            if(v & nm_hdmi::kCycleStop) ++stops;
            if(v & nm_hdmi::kClearError) ++resets;
        }
    }
    void delayUs(unsigned) { ++delays; }
};
static void fixChecksum(uint8_t *b) {
    unsigned sum=0;for(unsigned i=0;i<127;i++)sum+=b[i];
    b[127]=uint8_t(0-(sum&255));
}
int main() {
    Fake io;
    const uint8_t magic[]={0,255,255,255,255,255,255,0};
    std::memcpy(io.edid,magic,8);
    io.edid[8]=0x22; io.edid[9]=0xf0; io.edid[10]=0x7f; io.edid[11]=0x35;
    io.edid[126]=1;
    fixChecksum(io.edid);
    io.edid[128]=0x02; io.edid[129]=0x03; fixChecksum(io.edid+128);
    uint8_t out[256]={};size_t len=0;uint32_t status=0;
    assert(nm_hdmi::readEDID(io,out,len,status)==nm_hdmi::Status::ok);
    assert(len==256 && io.starts==2 && io.stops==2 && io.pin==0 && io.words==64);
    assert(std::memcmp(out,io.edid,256)==0);
    Fake bad=io; bad.edid[16] ^= 1; bad.pin=0; bad.starts=0; bad.words=0; bad.stops=0;
    assert(nm_hdmi::readEDID(bad,out,len,status)==nm_hdmi::Status::checksum);
    assert(len==0 && bad.pin==0 && bad.stops==1);
    Fake busy=io; busy.pin=0; busy.starts=0;busy.busy=true;
    assert(nm_hdmi::readEDID(busy,out,len,status)==nm_hdmi::Status::busy);
    assert(busy.starts==0 && busy.pin==0);
    Fake empty=io; empty.pin=0;empty.starts=0;empty.missing=true;
    assert(nm_hdmi::readEDID(empty,out,len,status)==nm_hdmi::Status::nak);
    assert(empty.pin==0 && empty.resets==1);
    Fake single=io; single.pin=0;single.starts=0;single.words=0;single.stops=0;
    single.edid[126]=0;fixChecksum(single.edid);
    assert(nm_hdmi::readEDID(single,out,len,status)==nm_hdmi::Status::ok);
    assert(len==128 && single.starts==1 && single.stops==1);
    Fake timeout=io; timeout.pin=0;timeout.starts=0;timeout.stalled=true;
    assert(nm_hdmi::readEDID(timeout,out,len,status)==nm_hdmi::Status::timeout);
    assert(timeout.pin==0 && timeout.delays>=400);
    Fake unavailable=io; unavailable.pin=0;unavailable.starts=0;unavailable.inaccessible=true;
    assert(nm_hdmi::readEDID(unavailable,out,len,status)==nm_hdmi::Status::inaccessible);
    assert(unavailable.pin==0 && unavailable.starts==0);
    std::puts("PASS: EDID base/CTA, checksum fail, busy, NAK, timeout, inaccessible, base-only");
}
