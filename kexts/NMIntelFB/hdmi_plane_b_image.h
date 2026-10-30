// SPDX-License-Identifier: MIT
// Meteor Lake HDMI B primary-plane register image for a pinned i915 GGTT
// scanout lease. Planning only: never write these registers until the full
// power/clock/DBUF/transcoder transaction owns pipe B and can roll it back.
#ifndef NMINTELFB_HDMI_PLANE_B_IMAGE_H
#define NMINTELFB_HDMI_PLANE_B_IMAGE_H
#include <stdint.h>
#include "xg_i915.h"

namespace nm_hdmi {
constexpr uint32_t kPlaneBImageABI = 0x48504231u; // HPB1
constexpr uint32_t kPlaneBRegisterCount = 6u;
constexpr uint32_t kPlaneBNoArmCount = 5u;
constexpr uint32_t kPlaneBWidth = 1920u;
constexpr uint32_t kPlaneBHeight = 1080u;
constexpr uint32_t kPlaneBPitch = 7680u;
constexpr uint64_t kPlaneBBytes = uint64_t(kPlaneBPitch) * kPlaneBHeight;

// Linux i915 display/skl_universal_plane_regs.h and
// display/skl_universal_plane.c. PLANE_SURF_B is the final arm/latch write.
// PLANE_CTL_FORMAT_XRGB_8888 = (4 << 24), linear tiling, 0 degree rotation.
struct PlaneBImage {
    uint32_t abi, count, noArmCount, reserved;
    uint32_t offsets[kPlaneBRegisterCount];
    uint32_t values[kPlaneBRegisterCount];
};
static_assert(sizeof(PlaneBImage) == (4u + 2u*kPlaneBRegisterCount)*4u,
              "Plane B register image must use fixed uint32 fields");

enum class PlaneBImageResult : uint32_t {
    ok, invalidSurface, unknownEDPRange, overlapsEDP, invalidGGTT
};

// Firmware eDP A is not owned by the HDMI backend: its entire active
// scanout GGTT byte range must be supplied and excluded, even if i915's
// own allocator also reserves the boot framebuffer.
inline PlaneBImageResult buildPlaneBImage(
    const xg_i915_hdmi_surface &surface,
    uint64_t edpStart, uint64_t edpBytes,
    PlaneBImage &out)
{
    out = {};
    if (surface.abi != XG_HDMI_SURFACE_ABI ||
        (surface.op != XG_HDMI_SURFACE_ALLOC &&
         surface.op != XG_HDMI_SURFACE_QUERY) ||
        surface.width != kPlaneBWidth || surface.height != kPlaneBHeight ||
        surface.pitch != kPlaneBPitch || surface.bytes != kPlaneBBytes ||
        surface.reserved != 0)
        return PlaneBImageResult::invalidSurface;
    constexpr uint64_t kGGTTLimit = 0x100000000ull;
    if (!edpBytes || edpStart >= kGGTTLimit ||
        edpBytes > kGGTTLimit - edpStart)
        return PlaneBImageResult::unknownEDPRange;
    // PLANE_SURF is page aligned. Current i915 pin uses 64KiB alignment.
    if (!surface.ggtt_offset || (surface.ggtt_offset & 0xffffu) ||
        surface.ggtt_offset >= kGGTTLimit ||
        surface.bytes > kGGTTLimit - surface.ggtt_offset)
        return PlaneBImageResult::invalidGGTT;
    const uint64_t end = surface.ggtt_offset + surface.bytes;
    if (surface.ggtt_offset < edpStart + edpBytes && edpStart < end)
        return PlaneBImageResult::overlapsEDP;

    out.abi = kPlaneBImageABI;
    out.count = kPlaneBRegisterCount;
    out.noArmCount = kPlaneBNoArmCount;
    const uint32_t offsets[kPlaneBRegisterCount] = {
        0x71188u, // PLANE_STRIDE_1_B
        0x7118cu, // PLANE_POS_1_B
        0x71190u, // PLANE_SIZE_1_B
        0x711a4u, // PLANE_OFFSET_1_B
        0x71180u, // PLANE_CTL_1_B
        0x7119cu  // PLANE_SURF_1_B: arm LAST, after full modeset setup
    };
    const uint32_t values[kPlaneBRegisterCount] = {
        kPlaneBPitch / 64u, // linear surface: stride in units of 64 bytes
        0u,                 // position (0,0)
        ((kPlaneBHeight-1u)<<16) | (kPlaneBWidth-1u),
        0u,                 // no source crop/offset
        (1u<<31) | (4u<<24), // enable XRGB8888; no tiling/decompression
        uint32_t(surface.ggtt_offset)
    };
    for (unsigned i=0; i<kPlaneBRegisterCount; ++i) {
        out.offsets[i]=offsets[i];
        out.values[i]=values[i];
    }
    return PlaneBImageResult::ok;
}
}
#endif
