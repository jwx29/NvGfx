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
    unsigned claims=0, releases=0, gmbus2Reads=0;
    uint32_t pin=0;
    bool missing=false, busy=false, active=false, acquired=false;
    bool stalled=false, inaccessible=false;
    // Reset controller and test bookkeeping without erasing the EDID.
    void resetTransfer() {
        pos=words=delays=resets=starts=stops=claims=releases=gmbus2Reads=0;
        pin=0; missing=busy=active=acquired=stalled=inaccessible=false;
    }
    uint32_t read32(uint32_t addr) {
        if (addr==nm_hdmi::kGMBUS0) return pin;
        if (addr==nm_hdmi::kGMBUS2) {
            ++gmbus2Reads;
            if (inaccessible) return 0xffffffffu;
            // Intel GMBUS2.INUSE: first read of 0 ACQUIRES the semaphore;
            // every subsequent read returns 1 until someone writes 1.
            const bool inUse=busy || acquired;
            if (!inUse) { acquired=true; ++claims; }
            uint32_t v=inUse ? nm_hdmi::kInUse : 0;
            if (active) v |= nm_hdmi::kActive;
            if (stalled) return v;
            if (missing) return v | nm_hdmi::kNAK;
            return v | nm_hdmi::kReady | nm_hdmi::kWaitPhase;
        }
        if (addr==nm_hdmi::kGMBUS3) {
            uint32_t v=0;
            for (unsigned i=0;i<4;i++) v|=uint32_t(edid[pos++])<<(8*i);
            ++words; return v;
        }
        return 0;
    }
    void write32(uint32_t addr,uint32_t v) {
        if (addr==nm_hdmi::kGMBUS0) pin=v;
        if (addr==nm_hdmi::kGMBUS2 && (v & nm_hdmi::kInUse)) {
            // A driver may only release a semaphore it acquired.
            assert(acquired && !busy);
            acquired=false; ++releases;
        }
        if (addr==nm_hdmi::kGMBUS1) {
            if (v & nm_hdmi::kCycleIndex) {
                assert(v & nm_hdmi::kCycleWait);
                ++starts; pos=(v>>8)&255;
            }
            if (v & nm_hdmi::kCycleStop) ++stops;
            if (v & nm_hdmi::kClearError) ++resets;
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
    assert(io.claims==2 && io.releases==2 && !io.acquired);
    assert(std::memcmp(out,io.edid,256)==0);
    // Repeat immediately to catch forgotten release after successful read.
    io.resetTransfer();
    assert(nm_hdmi::readEDID(io,out,len,status)==nm_hdmi::Status::ok);
    assert(io.claims==2 && io.releases==2 && !io.acquired);

    Fake bad=io;bad.resetTransfer();bad.edid[16]^=1;
    assert(nm_hdmi::readEDID(bad,out,len,status)==nm_hdmi::Status::checksum);
    assert(len==0 && bad.stops==1 && bad.claims==1 && bad.releases==1);

    Fake busy=io;busy.resetTransfer();busy.busy=true;
    assert(nm_hdmi::readEDID(busy,out,len,status)==nm_hdmi::Status::busy);
    assert(busy.starts==0 && busy.claims==0 && busy.releases==0 && busy.pin==0);

    Fake selected=io;selected.resetTransfer();selected.pin=4;
    assert(nm_hdmi::readEDID(selected,out,len,status)==nm_hdmi::Status::busy);
    assert(selected.pin==4 && selected.claims==1 && selected.releases==1 && !selected.acquired);

    Fake running=io;running.resetTransfer();running.active=true;
    assert(nm_hdmi::readEDID(running,out,len,status)==nm_hdmi::Status::busy);
    assert(running.claims==1 && running.releases==1 && !running.acquired);

    Fake empty=io;empty.resetTransfer();empty.missing=true;
    assert(nm_hdmi::readEDID(empty,out,len,status)==nm_hdmi::Status::nak);
    assert(empty.pin==0 && empty.resets==1 && empty.claims==1 && empty.releases==1);

    Fake single=io;single.resetTransfer();
    single.edid[126]=0;fixChecksum(single.edid);
    assert(nm_hdmi::readEDID(single,out,len,status)==nm_hdmi::Status::ok);
    assert(len==128 && single.starts==1 && single.stops==1 &&
           single.claims==1 && single.releases==1);

    Fake timeout=io;timeout.resetTransfer();timeout.stalled=true;
    assert(nm_hdmi::readEDID(timeout,out,len,status)==nm_hdmi::Status::timeout);
    assert(timeout.pin==0 && timeout.delays>=400 &&
           timeout.claims==1 && timeout.releases==1);

    Fake unavailable=io;unavailable.resetTransfer();unavailable.inaccessible=true;
    assert(nm_hdmi::readEDID(unavailable,out,len,status)==nm_hdmi::Status::inaccessible);
    assert(unavailable.pin==0 && unavailable.starts==0 &&
           unavailable.claims==0 && unavailable.releases==0);
    std::puts("PASS: read-to-acquire GMBUS semaphore and release on success/error; EDID/NAK/timeout/busy cases");
}
