// SPDX-License-Identifier: MIT
// Reversible MTL PW_2/PW_B request handling for a future HDMI B backend.
// This helper alone does NOT make the DDI B IO, DC_OFF, C10 PLL or PHY ready.
// Call ONLY while holding exclusive display-power ownership/serialization;
// the driver request bits are shared with other display users. Never enable
// this transaction directly from preflight, EDID probing or at startup.
#ifndef NMINTELFB_HDMI_MTL_POWER_LEASE_H
#define NMINTELFB_HDMI_MTL_POWER_LEASE_H
#include <stdint.h>
#include "hdmi_mtl_power.h"
namespace nm_hdmi {

constexpr uint32_t kMtlBPowerRequestMask =
    kPowerRequest(kPW2Index) | kPowerRequest(kPWBIndex);

enum class MtlBPowerLeaseResult : uint32_t {
    ok, alreadyAcquired, inaccessible, timeout, ownershipLost, rollbackFailed
};
inline const char *mtlBPowerLeaseResultName(MtlBPowerLeaseResult s) {
    switch(s) {
    case MtlBPowerLeaseResult::ok: return "ok";
    case MtlBPowerLeaseResult::alreadyAcquired: return "already-acquired";
    case MtlBPowerLeaseResult::inaccessible: return "power-mmio-inaccessible";
    case MtlBPowerLeaseResult::timeout: return "power-well-timeout";
    case MtlBPowerLeaseResult::ownershipLost: return "power-request-lost";
    case MtlBPowerLeaseResult::rollbackFailed: return "power-rollback-failed";
    }
    return "unknown";
}
struct MtlBPowerLease {
    uint32_t ownedRequests = 0;
    bool active = false;
};

template <typename IO>
MtlBPowerLeaseResult releaseMtlBPower(IO &io, MtlBPowerLease &lease) {
    // Driver request bits must only be cleared for wells acquired by us.
    // If a different controller writes the same bit concurrently, even
    // read-modify-write is unsafe: the caller MUST hold its shared lock.
    if (lease.ownedRequests) {
        const uint32_t current = io.read32(kPowerCtlDriver);
        if (current == 0xffffffffu)
            return MtlBPowerLeaseResult::rollbackFailed;
        io.write32(kPowerCtlDriver, current & ~lease.ownedRequests);
        const uint32_t check = io.read32(kPowerCtlDriver);
        if (check == 0xffffffffu || (check & lease.ownedRequests))
            return MtlBPowerLeaseResult::rollbackFailed;
    }
    lease = {};
    return MtlBPowerLeaseResult::ok;
}

template <typename IO>
MtlBPowerLeaseResult acquireMtlBPower(IO &io, MtlBPowerLease &lease,
                                       unsigned polls = 100u) {
    if (lease.active || lease.ownedRequests)
        return MtlBPowerLeaseResult::alreadyAcquired;
    const uint32_t bios = io.read32(kPowerCtlBios);
    if (bios == 0xffffffffu)
        return MtlBPowerLeaseResult::inaccessible;

    // PW_2 powers prerequisite display resources; only request PW_B
    // after PW_2's driver request AND hardware state are asserted.
    const uint32_t indices[] = {kPW2Index, kPWBIndex};
    for (unsigned i = 0; i != 2; ++i) {
        const uint32_t request = kPowerRequest(indices[i]);
        const uint32_t state = kPowerState(indices[i]);
        uint32_t current = io.read32(kPowerCtlDriver);
        if (current == 0xffffffffu) {
            const MtlBPowerLeaseResult rolled = releaseMtlBPower(io, lease);
            return rolled == MtlBPowerLeaseResult::ok ?
                   MtlBPowerLeaseResult::inaccessible : rolled;
        }
        if (!(current & request)) {
            // Preserve all unrelated requests and the read-only state bits.
            io.write32(kPowerCtlDriver, current | request);
            lease.ownedRequests |= request;
        }
        bool ready = false;
        MtlBPowerLeaseResult failure = MtlBPowerLeaseResult::timeout;
        for (unsigned j = 0; j < polls; ++j) {
            current = io.read32(kPowerCtlDriver);
            if (current == 0xffffffffu) {
                failure = MtlBPowerLeaseResult::inaccessible;
                break;
            }
            if (!(current & request)) {
                failure = MtlBPowerLeaseResult::ownershipLost;
                break;
            }
            if (current & state) {
                ready = true;
                break;
            }
            io.delayUs(50u);
        }
        if (!ready) {
            const MtlBPowerLeaseResult rolled = releaseMtlBPower(io, lease);
            return rolled == MtlBPowerLeaseResult::ok ? failure : rolled;
        }
    }
    lease.active = true;
    return MtlBPowerLeaseResult::ok;
}
} // namespace nm_hdmi
#endif
