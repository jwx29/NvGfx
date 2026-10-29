// SPDX-License-Identifier: MIT
// Hardware-free C10 HDMI PLL image programming/rollback simulation.
#include "hdmi_c10_pll_image.h"
#include <cassert>
#include <cstdio>
struct FakeBus {
    uint8_t regs[2][4096]={};
    unsigned writes=0,reads=0,failWriteAt=0,failReadAt=0;
    nm_hdmi::C10Result read(unsigned lane,uint16_t addr,uint8_t &val) {
        assert(lane<2 && addr<4096);
        if(++reads==failReadAt) return nm_hdmi::C10Result::timeout;
        if(addr>=nm_hdmi::kVdrPll0 && addr!=nm_hdmi::kVdrControl1 &&
           !(regs[lane][nm_hdmi::kVdrControl1] & nm_hdmi::kVdrAccess))
            return nm_hdmi::C10Result::ackError;
        val=regs[lane][addr];
        return nm_hdmi::C10Result::ok;
    }
    nm_hdmi::C10Result write(unsigned lane,uint16_t addr,uint8_t val,bool) {
        assert(lane<2 && addr<4096);
        if(++writes==failWriteAt) {
            failWriteAt=0; // transient failure; rollback can proceed
            return nm_hdmi::C10Result::timeout;
        }
        regs[lane][addr]=val;
        return nm_hdmi::C10Result::ok;
    }
    nm_hdmi::C10Result update(unsigned lane,uint16_t addr,
                             uint8_t mask,uint8_t set,bool committed) {
        uint8_t old=0;
        auto err=read(lane,addr,old);
        if(err!=nm_hdmi::C10Result::ok) return err;
        return write(lane,addr,uint8_t((old&~mask)|set),committed);
    }
};
int main() {
    using namespace nm_hdmi;
    FakeBus bus;
    bus.regs[0][kVdrControl1]=0x80;
    bus.regs[1][kVdrControl1]=0x40;
    bus.regs[0][kVdrCustomWidth]=3;
    bus.regs[1][kVdrCustomWidth]=2;
    for(unsigned i=0;i<20;i++) bus.regs[0][kVdrPll0+i]=uint8_t(i+11);
    C10PllProgrammer<FakeBus> program(bus);
    C10SavedState old;
    assert(program.program(kKnownHdmi1080p60C10,old)==C10ImageResult::ok);
    assert(old.valid);
    for(unsigned i=0;i<20;i++)
        assert(bus.regs[0][kVdrPll0+i]==kKnownHdmi1080p60C10.pll[i]);
    assert(bus.regs[0][kVdrCmn0]==0x01);
    assert(bus.regs[0][kVdrTx0]==0x10);
    assert((bus.regs[0][kVdrControl1] & 7u)==7u);
    assert(bus.regs[0][kVdrCustomWidth]==0 &&
           bus.regs[1][kVdrCustomWidth]==0);
    assert(program.rollback(old)==C10ImageResult::ok);
    for(unsigned i=0;i<20;i++)
        assert(bus.regs[0][kVdrPll0+i]==uint8_t(i+11));
    assert(bus.regs[0][kVdrControl1]==0x80);
    assert(bus.regs[1][kVdrControl1]==0x40);
    assert(bus.regs[0][kVdrCustomWidth]==3);
    assert(bus.regs[1][kVdrCustomWidth]==2);
    // The first timeout occurs mid-image; the saved state is restored
    // without enabling any real hardware.
    bus.failWriteAt=bus.writes+8;
    C10SavedState again;
    assert(program.program(kKnownHdmi1080p60C10,again)==C10ImageResult::writeFailure);
    for(unsigned i=0;i<20;i++)
        assert(bus.regs[0][kVdrPll0+i]==uint8_t(i+11));
    assert(bus.regs[0][kVdrControl1]==0x80);
    FakeBus noRead;
    noRead.failReadAt=5;
    C10PllProgrammer<FakeBus> p2(noRead);
    C10SavedState snap;
    assert(p2.program(kKnownHdmi1080p60C10,snap)==C10ImageResult::readFailure);
    // Intel requires the MSGBUS_ACCESS bit to be set *before* reading
    // vendor registers. The failed snapshot must restore that control
    // state, and must not change any PLL register.
    assert(noRead.writes==4);
    assert(noRead.regs[0][kVdrControl1]==0 &&
           noRead.regs[1][kVdrControl1]==0);
    for(unsigned i=0;i<20;i++)
        assert(noRead.regs[0][kVdrPll0+i]==0);
    std::puts("PASS: MTL C10 1080p60 PLL image, saved-state rollback and pre-write failures");
}
