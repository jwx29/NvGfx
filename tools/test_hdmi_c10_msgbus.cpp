// SPDX-License-Identifier: MIT
// Pure simulation of Meteor Lake C10 DDI B mailbox transactions.
#include "hdmi_c10_msgbus.h"
#include <cassert>
#include <cstdio>
struct C10Fake {
    uint32_t cmd[2]={}, ack[2]={};
    uint8_t vdr[4096]={};
    unsigned words=0,writes=0,delays=0;
    bool wrong=false,error=false,stuck=false,missing=false;
    uint32_t read32(uint32_t reg) {
        if(missing) return 0xffffffffu;
        if(reg==nm_hdmi::kC10BControl0||reg==nm_hdmi::kC10BControl0+4) {
            const unsigned i=(reg-nm_hdmi::kC10BControl0)/4;
            return cmd[i] | (stuck?nm_hdmi::kC10Pending:0u);
        }
        if(reg==nm_hdmi::kC10BStatus0||reg==nm_hdmi::kC10BStatus0+4) {
            const unsigned i=(reg-nm_hdmi::kC10BStatus0)/4;
            uint32_t v=ack[i];
            if(error) v|=nm_hdmi::kC10Error;
            if(wrong) v=(v&~(15u<<27))|(6u<<27);
            return v;
        }
        return 0;
    }
    void write32(uint32_t reg,uint32_t value) {
        if(reg==nm_hdmi::kC10BStatus0||reg==nm_hdmi::kC10BStatus0+4) {
            const unsigned i=(reg-nm_hdmi::kC10BStatus0)/4;
            if(value==nm_hdmi::kC10ResponseClear) ack[i]=0u;
            return;
        }
        if(reg==nm_hdmi::kC10BControl0||reg==nm_hdmi::kC10BControl0+4) {
            const unsigned i=(reg-nm_hdmi::kC10BControl0)/4;
            const uint32_t cmdType=(value>>27)&15u;
            const uint32_t address=value&0xfffu;
            cmd[i]=0; // emulate pending completion
            ++words;
            if(cmdType==nm_hdmi::kC10Read) {
                ack[i]=nm_hdmi::kC10Ready|(nm_hdmi::kC10ReadAck<<27)|
                      (uint32_t(vdr[address])<<16);
            } else if(cmdType==nm_hdmi::kC10WriteCommitted||
                      cmdType==nm_hdmi::kC10WriteUncommitted) {
                vdr[address]=uint8_t(value>>16);
                ++writes;
                if(cmdType==nm_hdmi::kC10WriteCommitted)
                    ack[i]=nm_hdmi::kC10Ready|(nm_hdmi::kC10WriteAck<<27);
            } else assert(false);
        }
    }
    void delayUs(unsigned) {++delays;}
};
int main() {
    using namespace nm_hdmi;
    C10Fake f; C10Bus<C10Fake> bus={f};
    assert(bus.write(0,0xc70u,4u,true)==C10Result::ok);
    uint8_t out=0;
    assert(bus.read(0,0xc70u,out)==C10Result::ok && out==4u);
    assert(bus.update(1,0xc70u,0,2u,true)==C10Result::ok);
    assert(bus.read(1,0xc70u,out)==C10Result::ok && out==6u);
    assert(bus.read(2,0xc70u,out)==C10Result::badArgs);
    f.wrong=true;
    assert(bus.read(0,0xc70u,out)==C10Result::ackType);
    f.wrong=false;f.error=true;
    assert(bus.read(0,0xc70u,out)==C10Result::ackError);
    f.error=false;f.stuck=true;
    assert(bus.read(0,0xc70u,out)==C10Result::timeout);
    f.stuck=false;f.missing=true;
    assert(bus.read(0,0xc70u,out)==C10Result::inaccessible);
    std::puts("PASS: C10 PHY B read/write/ACK, wrong-ACK, NAK, stalled and absent MMIO");
}
