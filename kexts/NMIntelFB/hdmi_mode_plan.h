// SPDX-License-Identifier: MIT
// Meteor Lake native HDMI B mode plan from a validated EDID.
//
// This is a hardware-independent plan, NEVER an executable MMIO script.
// Linux i915 uses additional display power, DDI C10 PHY/PLL, DBUF,
// watermarks, GGTT, HDMI infoframes, trans/pipe and plane programming.
// Do not write the entries here unless all of those are owned and ready.
#ifndef NMINTELFB_HDMI_MODE_PLAN_H
#define NMINTELFB_HDMI_MODE_PLAN_H
#include <stdint.h>
#include <stddef.h>
#include "hdmi_edid_timing.h"
namespace nm_hdmi {

constexpr uint32_t kModePlanABI = 0x484d5031u; // 'HMP1'
constexpr uint32_t kPlanTimingCount = 7;
constexpr uint32_t kPipeB = 1;
constexpr uint32_t kDDIB = 1;
constexpr uint32_t kC10PLL1 = 1;
constexpr uint32_t kHDMILanes = 4;
constexpr uint32_t kHDMIRefClockKHz = 38400;

// Linux i915's successful power-domain capture: always-on core +
// PW_2 and PW_B for the B pipe/transcoder, and DDI_B lanes / IO.
// DDI_IO_B power is controlled by MTL's PLL logic, NOT legacy ICL
// DDI_IO registers 0x45450/0x45454. These are REQUIREMENTS, not
// indications that the power wells are presently acquired.
enum ModeRequirement : uint32_t {
    requiresPW2 = 1u << 0,
    requiresPWB = 1u << 1,
    requiresDDIBLanes = 1u << 2,
    requiresDDIBIO = 1u << 3,
    requiresC10PHY = 1u << 4,
    requiresC10PLL = 1u << 5,
    requiresIndependentGGTT = 1u << 6,
    requiresDisplayHead5 = 1u << 7,
    requiresPipeDBUFWatermarks = 1u << 8,
    requiresHDMIInfoframes = 1u << 9
};
constexpr uint32_t kAllModeRequirements =
    requiresPW2 | requiresPWB | requiresDDIBLanes | requiresDDIBIO |
    requiresC10PHY | requiresC10PLL | requiresIndependentGGTT |
    requiresDisplayHead5 | requiresPipeDBUFWatermarks |
    requiresHDMIInfoframes;

struct ModePlan {
    uint32_t abi;
    uint32_t count;
    uint32_t pipe;
    uint32_t ddi;
    uint32_t pll;
    uint32_t lanes;
    uint32_t refClockKHz;
    uint32_t pixelClockHz;
    uint32_t requirements;
    uint32_t expectedRefreshMilliHz;
    uint32_t offsets[kPlanTimingCount];
    uint32_t values[kPlanTimingCount];
};
static_assert(sizeof(ModePlan) == (10 + 2*kPlanTimingCount)*sizeof(uint32_t),
              "ModePlan is a fixed little-endian uint32 ABI");

inline uint32_t packTiming(uint32_t end, uint32_t start) {
    return ((end - 1u) << 16) | (start - 1u);
}

// Only currently validated first test mode, with Linux-reference
// timing (1080p60, positive separate sync). NO port or clock writes.
inline bool buildModePlan(const Timing &m, ModePlan &out) {
    if (m.pixelClockHz != 148500000u ||
        m.hActive != 1920u || m.hSyncStart != 2008u ||
        m.hSyncEnd != 2052u || m.hTotal != 2200u ||
        m.vActive != 1080u || m.vSyncStart != 1084u ||
        m.vSyncEnd != 1089u || m.vTotal != 1125u ||
        (m.edidSyncFlags & 0x1eu) != 0x1eu)
        return false;

    out = {};
    out.abi=kModePlanABI;
    out.count=kPlanTimingCount;
    out.pipe=kPipeB;
    out.ddi=kDDIB;
    out.pll=kC10PLL1; // observed Linux allocation, not guaranteed free on macOS
    out.lanes=kHDMILanes;
    out.refClockKHz=kHDMIRefClockKHz;
    out.pixelClockHz=m.pixelClockHz;
    out.requirements=kAllModeRequirements;
    out.expectedRefreshMilliHz=60000u;
    const uint32_t offsets[kPlanTimingCount] = {
        0x61000u, // TRANS_HTOTAL_B
        0x61004u, // TRANS_HBLANK_B
        0x61008u, // TRANS_HSYNC_B
        0x6100cu, // TRANS_VTOTAL_B
        0x61010u, // TRANS_VBLANK_B
        0x61014u, // TRANS_VSYNC_B
        0x6101cu  // PIPEBSRC
    };
    const uint32_t values[kPlanTimingCount] = {
        packTiming(m.hTotal,m.hActive),
        packTiming(m.hTotal,m.hActive),
        packTiming(m.hSyncEnd,m.hSyncStart),
        packTiming(m.vTotal,m.vActive),
        // ADL+ TRANS_VBLANK_START is ignored; i915 clears to 1
        // and separately programs TRANS_SET_CONTEXT_LATENCY.
        packTiming(m.vTotal,1u),
        packTiming(m.vSyncEnd,m.vSyncStart),
        packTiming(m.hActive,m.vActive)
    };
    for (uint32_t i=0;i<kPlanTimingCount;i++) {
        out.offsets[i]=offsets[i];
        out.values[i]=values[i];
    }
    return true;
}

} // namespace nm_hdmi
#endif
