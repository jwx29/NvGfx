// SPDX-License-Identifier: MIT
// Meteor Lake C10 HDMI B PLL *vendor-register* programming layer.
// Sources: Linux i915 intel_cx0_phy.c: intel_c10_pll_program(),
// intel_cx0_phy_regs.h, and verified 1080p60 C10 DPLL1 debugfs image.
// NEVER call unless a driver holds DC_OFF, quiesced PSR, exclusive DDI B
// PHY / DPLL1 ownership, and has a bounded shutdown path. This layer
// alone does not enable a PLL or transmitter.
#ifndef NMINTELFB_HDMI_C10_PLL_IMAGE_H
#define NMINTELFB_HDMI_C10_PLL_IMAGE_H
#include <stdint.h>
#include "hdmi_c10_msgbus.h"
namespace nm_hdmi {
struct C10PllImage {
    uint8_t pll[20];
    uint8_t cmn, tx;
};
constexpr C10PllImage kKnownHdmi1080p60C10 = {{
    0xf4,0x00,0x7a,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x00,0x00,0x58,0x00,0x00,0x0a,
    0x06,0x0f,0x85,0x23
},0x01,0x10};

constexpr uint16_t kVdrPll0=0xc00u;
constexpr uint16_t kVdrCmn0=0xc20u;
constexpr uint16_t kVdrTx0=0xc30u;
constexpr uint16_t kVdrControl1=0xc70u;
constexpr uint16_t kVdrCustomWidth=0xd02u;
constexpr uint8_t kVdrAccess=1u<<2;
constexpr uint8_t kVdrMaster=1u<<1;
constexpr uint8_t kVdrUpdate=1u<<0;

enum class C10ImageResult : uint32_t {
    ok, readFailure, writeFailure, rollbackFailure
};
struct C10SavedState {
    C10PllImage original={};
    uint8_t ctrl[2]={}, width[2]={};
    bool valid=false;
};

template <typename Bus> class C10PllProgrammer {
    Bus &bus;
    static bool good(C10Result r) { return r==C10Result::ok; }
public:
    explicit C10PllProgrammer(Bus &b):bus(b){}
    // All affected values must be readable BEFORE any modification.
    C10ImageResult snapshot(C10SavedState &saved) {
        saved.valid=false;
        if(!good(bus.read(0,kVdrControl1,saved.ctrl[0])) ||
           !good(bus.read(1,kVdrControl1,saved.ctrl[1])))
            return C10ImageResult::readFailure;
        // Per i915 intel_c10pll_readout_hw_state, MSGBUS_ACCESS must
        // be set BEFORE PLL vendor-register reads are possible. This is
        // a PHY write, so the caller must already hold exclusive power,
        // DC_OFF and PSR ownership. Keep the old control bits for undo.
        if(!good(bus.update(0,kVdrControl1,0,kVdrAccess,true)) ||
           !good(bus.update(1,kVdrControl1,0,kVdrAccess,true)))
            goto read_failed;
        if(!good(bus.read(0,kVdrCustomWidth,saved.width[0])) ||
           !good(bus.read(1,kVdrCustomWidth,saved.width[1])))
            goto read_failed;
        for(unsigned i=0;i<20;i++)
            if(!good(bus.read(0,uint16_t(kVdrPll0+i),saved.original.pll[i])))
                goto read_failed;
        if(!good(bus.read(0,kVdrCmn0,saved.original.cmn)) ||
           !good(bus.read(0,kVdrTx0,saved.original.tx)))
            goto read_failed;
        saved.valid=true;
        return C10ImageResult::ok;
read_failed:
        // Even if the snapshot fails, restore the original access bits
        // before returning (and report a failed rollback distinctly).
        {
            bool reverted=true;
            if(!good(bus.write(0,kVdrControl1,saved.ctrl[0],true)))
                reverted=false;
            if(!good(bus.write(1,kVdrControl1,saved.ctrl[1],true)))
                reverted=false;
            return reverted ? C10ImageResult::readFailure :
                              C10ImageResult::rollbackFailure;
        }
    }
    // Writes should be followed by the outer PHY/PLL lock sequencing.
    // This method is useful only when a future backend owns DDI B.
    C10ImageResult program(const C10PllImage &target, C10SavedState &saved) {
        const C10ImageResult snapshotResult=snapshot(saved);
        if(snapshotResult!=C10ImageResult::ok) return snapshotResult;
        if(!good(bus.update(0,kVdrControl1,0,kVdrAccess,true)) ||
           !good(bus.update(1,kVdrControl1,0,kVdrAccess,true)))
            goto undo;
        for(unsigned i=0;i<20;i++)
            if(!good(bus.write(0,uint16_t(kVdrPll0+i),
                              target.pll[i],(i%4u)==0u)))
                goto undo;
        if(!good(bus.write(0,kVdrCmn0,target.cmn,true)) ||
           !good(bus.write(0,kVdrTx0,target.tx,true)) ||
           !good(bus.update(0,kVdrCustomWidth,3u,0,true)) ||
           !good(bus.update(1,kVdrCustomWidth,3u,0,true)) ||
           !good(bus.update(0,kVdrControl1,0,
                            kVdrMaster|kVdrUpdate,true)))
            goto undo;
        return C10ImageResult::ok;
undo:
        return rollback(saved)==C10ImageResult::ok ?
            C10ImageResult::writeFailure : C10ImageResult::rollbackFailure;
    }
    C10ImageResult rollback(const C10SavedState &saved) {
        if(!saved.valid) return C10ImageResult::readFailure;
        bool okay=true;
        // Only invoke after the external port / transcoder are off.
        // Treat every restore failure as fatal to the owning backend.
        for(unsigned i=0;i<20;i++)
            if(!good(bus.write(0,uint16_t(kVdrPll0+i),
                              saved.original.pll[i],(i%4u)==0u)))
                okay=false;
        // Don't short-circuit: each restore must be attempted even if a
        // previous write failed, so the backend can report the final state.
        if(!good(bus.write(0,kVdrCmn0,saved.original.cmn,true))) okay=false;
        if(!good(bus.write(0,kVdrTx0,saved.original.tx,true))) okay=false;
        if(!good(bus.write(0,kVdrCustomWidth,saved.width[0],true))) okay=false;
        if(!good(bus.write(1,kVdrCustomWidth,saved.width[1],true))) okay=false;
        if(!good(bus.write(0,kVdrControl1,saved.ctrl[0],true))) okay=false;
        if(!good(bus.write(1,kVdrControl1,saved.ctrl[1],true))) okay=false;
        return okay ? C10ImageResult::ok:C10ImageResult::rollbackFailure;
    }
};
} // namespace nm_hdmi
#endif
