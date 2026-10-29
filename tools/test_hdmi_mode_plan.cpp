// SPDX-License-Identifier: MIT
// Portable test for a 1080p60 timing-only XeGfx HDMI B mode plan.
// The test asserts Linux i915's known-good MTL pipe B timing encodings;
// it cannot write hardware registers.
#include "hdmi_mode_plan.h"
#include <cassert>
#include <cstdio>
int main() {
    using namespace nm_hdmi;
    const Timing t = {
        148500000u, 1920u, 2008u, 2052u, 2200u,
        1080u, 1084u, 1089u, 1125u, 0x1eu
    };
    ModePlan p={};
    assert(buildModePlan(t,p));
    assert(sizeof(p)==96 && p.abi==kModePlanABI);
    assert(p.count==7 && p.pipe==1 && p.ddi==1 && p.pll==1);
    assert(p.lanes==4 && p.refClockKHz==38400u);
    assert(p.pixelClockHz==148500000u && p.expectedRefreshMilliHz==60000u);
    assert(p.requirements==kAllModeRequirements);
    const uint32_t offsets[]={0x61000,0x61004,0x61008,0x6100c,
                              0x61010,0x61014,0x6101c};
    const uint32_t values[]={0x0897077fu,0x0897077fu,0x080307d7u,
                             0x04640437u,0x04640000u,0x0440043bu,
                             0x077f0437u};
    for(unsigned i=0;i<7;i++) {
        assert(p.offsets[i]==offsets[i]);
        assert(p.values[i]==values[i]);
        assert(p.offsets[i]<0x70000u); // no port/DDI/PLL enable writes
    }
    Timing bad=t;
    bad.pixelClockHz=346500000u;
    assert(!buildModePlan(bad,p));
    bad=t;bad.vTotal=1124;
    assert(!buildModePlan(bad,p));
    bad=t;bad.edidSyncFlags=0x18;
    assert(!buildModePlan(bad,p));
    std::puts("PASS: Linux-matched MTL 1080p60 HDMI B timing plan; no MMIO writes");
}
