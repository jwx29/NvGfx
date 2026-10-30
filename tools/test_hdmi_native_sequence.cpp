// SPDX-License-Identifier: MIT
// Purely simulated Meteor Lake HDMI transaction tests; NO GPU access.
#include "hdmi_native_sequence.h"
#include <cassert>
#include <cstdio>
#include <cstring>
struct SimulatedNative {
    bool enabled[11]={};
    unsigned next=0, rolled=0, applied=0, verified=0;
    bool panel=true, bIdle=true, allStages=true;
    int badApply=-1, badVerify=-1;
    bool panelAlive() const { return panel; }
    bool portBIdle() const { return bIdle; }
    bool portBStillOurs(uint32_t) const { return bIdle; }
    bool supports(nm_hdmi::NativeStage s) const {
        return allStages || s != nm_hdmi::NativeStage::c10Pll;
    }
    bool apply(nm_hdmi::NativeStage s, const nm_hdmi::ModePlan &m) {
        const unsigned i=unsigned(s);
        assert(m.pixelClockHz==148500000u);
        assert(i==next); // must respect hardware sequence
        // MTL requires DDI_CTL_DE enabled before transcoder enable.
        if(s==nm_hdmi::NativeStage::enableTranscoderB)
            assert(enabled[unsigned(nm_hdmi::NativeStage::enablePortB)]);
        if(int(i)==badApply) return false;
        enabled[i]=true; ++next; ++applied; return true;
    }
    bool verify(nm_hdmi::NativeStage s, const nm_hdmi::ModePlan &) {
        ++verified;return int(s)!=badVerify;
    }
    void rollback(nm_hdmi::NativeStage s) {
        const unsigned i=unsigned(s);
        assert(next==i+1u);
        assert(enabled[i]);enabled[i]=false;--next;++rolled;
    }
};
int main() {
    using namespace nm_hdmi;
    Timing t={148500000,1920,2008,2052,2200,1080,1084,1089,1125,0x1e};
    ModePlan p={};assert(buildModePlan(t,p));
    UnimplementedNativeBackend unavailable;
    const NativeReport disabled=bringUp(unavailable,p);
    assert(disabled.result==NativeResult::noBackend);
    assert(disabled.failedStage==NativeStage::powerWells);
    assert(disabled.appliedCount==0 && disabled.rolledBackCount==0);
    SimulatedNative h;
    h.allStages=false;
    NativeReport r=bringUp(h,p);
    assert(r.result==NativeResult::noBackend &&
           r.failedStage==NativeStage::c10Pll && h.applied==0);
    h.allStages=true;
    h.badApply=unsigned(NativeStage::enablePortB);
    r=bringUp(h,p);
    assert(r.result==NativeResult::applyFailed);
    assert(r.failedStage==NativeStage::enablePortB);
    assert(r.appliedCount==0 && r.rolledBackCount==7);
    assert(h.next==0 && h.rolled==7);
    h=SimulatedNative{};
    h.badVerify=unsigned(NativeStage::enableTranscoderB);
    r=bringUp(h,p);
    assert(r.result==NativeResult::verifyFailed);
    assert(r.rolledBackCount==9 && h.next==0);
    h=SimulatedNative{};
    r=bringUp(h,p);
    assert(r.result==NativeResult::ok && r.appliedCount==11);
    assert(r.rolledBackCount==0);
    h=SimulatedNative{};
    h.bIdle=false;
    r=bringUp(h,p);
    assert(r.result==NativeResult::BAlreadyOwned && h.applied==0);
    h=SimulatedNative{};
    h.panel=false;
    r=bringUp(h,p);
    assert(r.result==NativeResult::eDPNotAlive && h.applied==0);
    p.pll=0;
    h=SimulatedNative{};
    r=bringUp(h,p);
    assert(r.result==NativeResult::invalidPlan && h.applied==0);
    std::puts("PASS: HDMI power/GGTT/C10/pipe/port/head5 order, fail closed, reversed rollback");
}
