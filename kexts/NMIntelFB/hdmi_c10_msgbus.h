// SPDX-License-Identifier: MIT
// Intel MTL display v14 C10 PHY B msgbus transport (DDI B only).
// Ported from Linux i915 intel_cx0_phy.c / intel_cx0_phy_regs.h.
//
// The owner MUST FIRST: disable DC5/6 (DC_OFF wakeref), pause PSR,
// obtain all DDI B PHY/power resources and a lock excluding i915 and
// other display controllers. Never call these on firmware eDP A.
// No automatic calls from NMIntelFB; NOT a complete HDMI PHY driver.
#ifndef NMINTELFB_HDMI_C10_MSGBUS_H
#define NMINTELFB_HDMI_C10_MSGBUS_H
#include <stdint.h>
namespace nm_hdmi {
constexpr uint32_t kC10BControl0=0x64140u;
constexpr uint32_t kC10BStatus0=0x64148u;
constexpr uint32_t kC10BTimer0=0x641d8u;
constexpr uint32_t kC10Pending=1u<<31;
constexpr uint32_t kC10Reset=1u<<15;
constexpr uint32_t kC10Ready=1u<<31;
constexpr uint32_t kC10Error=1u<<15;
constexpr uint32_t kC10CommandShift=27;
constexpr uint32_t kC10WriteUncommitted=1u;
constexpr uint32_t kC10WriteCommitted=2u;
constexpr uint32_t kC10Read=3u;
constexpr uint32_t kC10ReadAck=4u;
constexpr uint32_t kC10WriteAck=5u;
constexpr uint32_t kC10ResponseClear=(1u<<31)|(1u<<15);
constexpr uint32_t kC10VDRControl=0xc70u;
constexpr uint8_t kC10Access=4u;
constexpr uint8_t kC10Update=1u;
constexpr uint8_t kC10Master=2u;
enum class C10Result : uint32_t {
    ok, badArgs, inaccessible, previousBusy, timeout, ackError, ackType
};
inline const char *c10ResultName(C10Result r) {
    switch(r) {
    case C10Result::ok: return "ok";
    case C10Result::badArgs: return "invalid-address-or-lane";
    case C10Result::inaccessible: return "inaccessible";
    case C10Result::previousBusy: return "previous-transaction-busy";
    case C10Result::timeout: return "msgbus-timeout";
    case C10Result::ackError: return "msgbus-error";
    case C10Result::ackType: return "wrong-msgbus-ack";
    }
    return "unknown";
}
template <typename IO>
struct C10Bus {
    IO &io;
    // Exact MTL PHY B lane0/1 MMIO, not Xe2 remapped TC offsets.
    static uint32_t control(unsigned lane) { return kC10BControl0+4u*lane; }
    static uint32_t status(unsigned lane) { return kC10BStatus0+4u*lane; }

    bool pollClear(uint32_t reg, uint32_t mask, unsigned loops=20) {
        for(unsigned i=0;i<loops;++i) {
            uint32_t r=io.read32(reg);
            if(r==0xffffffffu) return false;
            if(!(r&mask)) return true;
            io.delayUs(50);
        }
        return false;
    }
    bool pollSet(uint32_t reg, uint32_t mask, uint32_t &out, unsigned loops=20) {
        for(unsigned i=0;i<loops;++i) {
            out=io.read32(reg);
            if(out==0xffffffffu) return false;
            if(out&mask) return true;
            io.delayUs(50);
        }
        return false;
    }
    // MTL i915 resets the C10 message bus after each completed
    // read/write to settle the PHY, then clears response W1C flags.
    C10Result settle(unsigned lane) {
        io.write32(control(lane),kC10Reset);
        if(!pollClear(control(lane),kC10Reset))
            return C10Result::timeout;
        io.write32(status(lane),kC10ResponseClear);
        return C10Result::ok;
    }
    C10Result transact(unsigned lane, uint16_t address, uint8_t value,
                       uint32_t cmd, uint8_t *result) {
        if(lane>1u || address>0xfffu || cmd<1 || cmd>3)
            return C10Result::badArgs;
        const uint32_t ctl=control(lane), sta=status(lane);
        uint32_t before=io.read32(ctl);
        if(before==0xffffffffu) return C10Result::inaccessible;
        if(before&kC10Pending) return C10Result::previousBusy;
        // W1C response ready/error, just like i915.
        io.write32(sta,kC10ResponseClear);
        uint32_t v=kC10Pending|(cmd<<kC10CommandShift)|
                   (uint32_t(value)<<16)|uint32_t(address);
        io.write32(ctl,v);
        // Linux reads wait for a READ_ACK directly. Committed and
        // uncommitted writes wait for the pending bit to deassert.
        if(cmd!=kC10Read && !pollClear(ctl,kC10Pending))
            return C10Result::timeout;
        const uint32_t ackCmd=cmd==kC10Read ? kC10ReadAck :
                               cmd==kC10WriteCommitted ? kC10WriteAck : 0;
        if(ackCmd) {
            uint32_t ack=0;
            if(!pollSet(sta,kC10Ready,ack)) return C10Result::timeout;
            if(ack&kC10Error) return C10Result::ackError;
            if(((ack>>kC10CommandShift)&15u)!=ackCmd)
                return C10Result::ackType;
            if(result) *result=uint8_t(ack>>16);
        } else {
            uint32_t ack=io.read32(sta);
            if(ack==0xffffffffu) return C10Result::inaccessible;
            if(ack&kC10Error) return C10Result::ackError;
        }
        io.write32(sta,kC10ResponseClear);
        return settle(lane);
    }
    C10Result read(unsigned lane, uint16_t address, uint8_t &result) {
        return transact(lane,address,0u,kC10Read,&result);
    }
    C10Result write(unsigned lane,uint16_t address,uint8_t val,bool committed) {
        return transact(lane,address,val,
                        committed?kC10WriteCommitted:kC10WriteUncommitted,
                        nullptr);
    }
    // Caller holds DC_OFF and PHY ownership. This bit only opens the
    // C10 VDR message bus; it does not power/configure or enable HDMI.
    C10Result update(unsigned lane,uint16_t address,uint8_t mask,
                     uint8_t set,bool committed) {
        uint8_t old=0;
        C10Result e=read(lane,address,old);
        if(e!=C10Result::ok) return e;
        const uint8_t next=uint8_t((old&~mask)|set);
        return next==old ? C10Result::ok : write(lane,address,next,committed);
    }
};
} // namespace nm_hdmi
#endif
