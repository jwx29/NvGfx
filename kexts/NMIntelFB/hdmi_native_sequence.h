// SPDX-License-Identifier: MIT
// XeGfx / Meteor Lake HDMI B: fail-closed native display bring-up
// transaction. Generic logic only; actual kernel display primitives are
// provided by an explicitly implemented backend. This does not write MMIO.
//
// A valid EDID/timing plan is not enough: every phase needs a working,
// rollback-capable backend before *any* destructive stage may start.
// Never allow this runner to borrow firmware eDP A or its scanout.
#ifndef NMINTELFB_HDMI_NATIVE_SEQUENCE_H
#define NMINTELFB_HDMI_NATIVE_SEQUENCE_H
#include <stdint.h>
#include <stddef.h>
#include "hdmi_mode_plan.h"

namespace nm_hdmi {
enum class NativeStage : uint32_t {
    powerWells = 0,    // PW_2 + PW_B + DDI B lane/IO + DC_OFF
    scanout,           // distinct pinned GGTT object + CPU access; no A overlap
    c10Pll,            // DPLL1 arbitration, MTL C10 message bus and lock
    c10Phy,            // DDI B C10 transmitter setup/levels
    pipeBConfig,       // 1080p60 timing, transcoder format, DBUF, watermarks
    planeBConfig,      // true GGTT offset, XRGB plane + stride/size
    hdmiPackets,       // HDMI infoframes, sink settings, audio off initially
    enablePortB,       // MTL+: DDI_CTL_DE_B enabled BEFORE transcoder
    enableTranscoderB, // MTL+: TRANS_CONF_B enabled AFTER port
    verifyFrames,      // bounded pipe B frame-count progress
    publishHead5,      // distinct IOFramebuffer + copy destination + VBL
    count
};

constexpr uint32_t kNativeStageCount = uint32_t(NativeStage::count);
constexpr NativeStage kNativeStages[] = {
    NativeStage::powerWells, NativeStage::scanout, NativeStage::c10Pll,
    NativeStage::c10Phy, NativeStage::pipeBConfig, NativeStage::planeBConfig,
    NativeStage::hdmiPackets, NativeStage::enablePortB,
    NativeStage::enableTranscoderB, NativeStage::verifyFrames,
    NativeStage::publishHead5
};
static_assert(kNativeStageCount == sizeof(kNativeStages)/sizeof(kNativeStages[0]),
              "Every native HDMI stage must be accounted for");

inline const char *stageName(NativeStage s) {
    switch (s) {
    case NativeStage::powerWells: return "power-wells";
    case NativeStage::scanout: return "ggtt-scanout";
    case NativeStage::c10Pll: return "c10-dpll1";
    case NativeStage::c10Phy: return "c10-phy-b";
    case NativeStage::pipeBConfig: return "pipe-b-config";
    case NativeStage::planeBConfig: return "plane-b-config";
    case NativeStage::hdmiPackets: return "hdmi-infoframes";
    case NativeStage::enablePortB: return "enable-ddi-b";
    case NativeStage::enableTranscoderB: return "enable-transcoder-b";
    case NativeStage::verifyFrames: return "verify-b-vblank";
    case NativeStage::publishHead5: return "publish-iofb-head5";
    default: return "unknown-stage";
    }
}

enum class NativeResult : uint32_t {
    ok,
    invalidPlan,
    eDPNotAlive,
    BAlreadyOwned,
    noBackend,
    applyFailed,
    verifyFailed
};
inline const char *nativeResultName(NativeResult r) {
    switch (r) {
    case NativeResult::ok: return "ok";
    case NativeResult::invalidPlan: return "invalid-timing-plan";
    case NativeResult::eDPNotAlive: return "edp-a-not-alive";
    case NativeResult::BAlreadyOwned: return "ddi-b-already-owned";
    case NativeResult::noBackend: return "native-backend-missing";
    case NativeResult::applyFailed: return "stage-apply-failed";
    case NativeResult::verifyFailed: return "stage-verification-failed";
    }
    return "unknown-result";
}
struct NativeReport {
    NativeResult result = NativeResult::noBackend;
    NativeStage failedStage = NativeStage::powerWells;
    uint32_t appliedCount = 0, rolledBackCount = 0;
};

// Backend contract:
//   supports(stage): phase and its precise UNDO are implemented and
//                    verified for THIS PCI revision and silicon stepping.
//   panelAlive(): eDP pipe A remains scanning out.
//   portBIdle(): no another owner of DDI B/pipe B.
//   apply(stage,plan): bounded; never writes A's pipe/PLL/GGTT.
//   verify(stage,plan): readback/timeout/lock validation (no blind sleep).
//   rollback(stage): idempotent and safe, reverse order; no A writes.
//
// The runner refuses to start until *every* phase is implemented;
// a partial backend CANNOT leave a half-enabled display behind.
template <typename Backend>
NativeReport bringUp(Backend &hw, const ModePlan &mode) {
    NativeReport report = {};
    if (mode.abi != kModePlanABI || mode.count != kPlanTimingCount ||
        mode.pipe != 1u || mode.ddi != 1u || mode.pll != 1u ||
        mode.lanes != 4u || mode.refClockKHz != 38400u ||
        mode.pixelClockHz != 148500000u ||
        mode.requirements != kAllModeRequirements) {
        report.result = NativeResult::invalidPlan;
        return report;
    }
    if (!hw.panelAlive()) {
        report.result = NativeResult::eDPNotAlive;
        return report;
    }
    if (!hw.portBIdle()) {
        report.result = NativeResult::BAlreadyOwned;
        return report;
    }
    for (uint32_t i=0; i<kNativeStageCount; ++i) {
        if (!hw.supports(kNativeStages[i])) {
            report.failedStage = kNativeStages[i];
            report.result = NativeResult::noBackend;
            return report; // no side effects
        }
    }
    // Real hardware may change after preflight. A single device lock is
    // required across this entire transaction, and each backend stage
    // must independently verify its own hardware ownership.
    for (uint32_t i=0; i<kNativeStageCount; ++i) {
        const NativeStage stage = kNativeStages[i];
        if (!hw.panelAlive() || !hw.portBStillOurs(i)) {
            report.result = NativeResult::eDPNotAlive;
            report.failedStage = stage;
            break;
        }
        if (!hw.apply(stage, mode)) {
            report.result = NativeResult::applyFailed;
            report.failedStage = stage;
            // apply() is required to be internally atomic on failure;
            // it must undo any partial changes within its own phase.
            break;
        }
        ++report.appliedCount;
        if (!hw.verify(stage, mode)) {
            report.result = NativeResult::verifyFailed;
            report.failedStage = stage;
            break;
        }
        // Never declare success, or continue to the next stage, after
        // an operation silently stalls the firmware-owned eDP screen.
        if (!hw.panelAlive()) {
            report.result = NativeResult::eDPNotAlive;
            report.failedStage = stage;
            break;
        }
        if (i + 1 == kNativeStageCount) {
            report.result = NativeResult::ok;
            return report;
        }
    }

    // Failure rollback in reverse order, the only ordering valid once the
    // external transmitter may have been enabled. Do not undo operations
    // from a different owner or touch firmware pipe A. The backend remains
    // responsible for handling failed/timeout rollback safely.
    while (report.appliedCount) {
        const NativeStage stage = kNativeStages[report.appliedCount-1];
        hw.rollback(stage);
        --report.appliedCount;
        ++report.rolledBackCount;
    }
    return report;
}

// Used by the current kext until real MTL power/C10/GGTT/head5 backend
// exists. Everything fails closed; cannot accidentally enable HDMI.
struct UnimplementedNativeBackend {
    bool panelAlive() const { return true; }
    bool portBIdle() const { return true; }
    bool portBStillOurs(uint32_t) const { return true; }
    bool supports(NativeStage) const { return false; }
    bool apply(NativeStage, const ModePlan &) { return false; }
    bool verify(NativeStage, const ModePlan &) const { return false; }
    void rollback(NativeStage) {}
};
} // namespace nm_hdmi
#endif
