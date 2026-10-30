// SPDX-License-Identifier: MIT
// Standalone plane B image tests. No MMIO and no graphics hardware used.
#include "hdmi_plane_b_image.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace nm_hdmi;
    xg_i915_hdmi_surface fb = {};
    fb.abi=XG_HDMI_SURFACE_ABI;
    fb.op=XG_HDMI_SURFACE_ALLOC;
    fb.width=kPlaneBWidth;
    fb.height=kPlaneBHeight;
    fb.pitch=kPlaneBPitch;
    fb.bytes=kPlaneBBytes;
    fb.ggtt_offset=0x2000000ull;
    PlaneBImage image={};
    // Firmware eDP A occupies a distinct 1920x1200 linear boot scanout.
    assert(buildPlaneBImage(fb,0x100000u,1920u*1200u*4u,image)
           ==PlaneBImageResult::ok);
    assert(image.abi==kPlaneBImageABI && image.count==6u);
    assert(image.noArmCount==5u);
    const uint32_t regs[]={0x71188u,0x7118cu,0x71190u,0x711a4u,
                           0x71180u,0x7119cu};
    const uint32_t values[]={120u,0u,0x0437077fu,0u,0x84000000u,
                             0x02000000u};
    for(unsigned i=0;i<6;++i){
        assert(image.offsets[i]==regs[i]);
        assert(image.values[i]==values[i]);
    }
    assert(image.offsets[image.count-1]==0x7119cu); // surface latches
    // Captured Meteor Lake values: firmware eDP 1920x1200 near the
    // high GGTT limit, separate HDMI allocation near the low end.
    fb.ggtt_offset=0x00ae0000ull;
    assert(buildPlaneBImage(fb,0xff600000ull,9216000ull,image)
           ==PlaneBImageResult::ok);
    assert(image.values[5]==0x00ae0000u);
    assert(image.values[4]==0x84000000u);
    fb.ggtt_offset=0x02000000ull;
    fb.op=XG_HDMI_SURFACE_QUERY;
    assert(buildPlaneBImage(fb,0x100000u,0x900000u,image)
           ==PlaneBImageResult::ok);
    fb.pitch=0;
    assert(buildPlaneBImage(fb,0x100000u,0x900000u,image)
           ==PlaneBImageResult::invalidSurface && image.abi==0u);
    fb.pitch=kPlaneBPitch;
    fb.bytes--;
    assert(buildPlaneBImage(fb,0x100000u,0x900000u,image)
           ==PlaneBImageResult::invalidSurface);
    fb.bytes=kPlaneBBytes;
    assert(buildPlaneBImage(fb,0,0,image)
           ==PlaneBImageResult::unknownEDPRange);
    assert(buildPlaneBImage(fb,0xffff0000ull,0x20000u,image)
           ==PlaneBImageResult::unknownEDPRange);
    assert(buildPlaneBImage(fb,0x2000000ull,0x1000u,image)
           ==PlaneBImageResult::overlapsEDP);
    assert(buildPlaneBImage(fb,0x2001000ull,0x1000u,image)
           ==PlaneBImageResult::overlapsEDP);
    fb.ggtt_offset=0x2001000ull;
    assert(buildPlaneBImage(fb,0x100000u,0x900000u,image)
           ==PlaneBImageResult::invalidGGTT);
    fb.ggtt_offset=0xffff0000ull;
    assert(buildPlaneBImage(fb,0x100000u,0x900000u,image)
           ==PlaneBImageResult::invalidGGTT);
    fb.ggtt_offset=0;
    assert(buildPlaneBImage(fb,0x100000u,0x900000u,image)
           ==PlaneBImageResult::invalidGGTT);
    fb.ggtt_offset=0x2000000ull;
    fb.reserved=1;
    assert(buildPlaneBImage(fb,0x100000u,0x900000u,image)
           ==PlaneBImageResult::invalidSurface);
    std::puts("PASS: HDMI pipe B 1080p XRGB8888 image, arm order, EDID-independent GGTT bounds and eDP isolation");
}
