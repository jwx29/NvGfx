// SPDX-License-Identifier: MIT
// Meteor Lake / Xe-LPD+ main display power wells for external DDI B.
//
// Verified against i915 intel_display_power_map.c + intel_display_regs.h:
// PW_2 index ICL_PW_CTL_IDX_PW_2=1; PW_B index XELPD_PW_CTL_IDX_PW_B=6.
// Each index has driver request (bit 2n+1), status (bit 2n).
// DO NOT clear or change firmware-owned or other kext-owned requests.
// Power-well state is not the C10 PHY PLL / DDI IO ownership protocol.
#ifndef NMINTELFB_HDMI_MTL_POWER_H
#define NMINTELFB_HDMI_MTL_POWER_H
#include <stdint.h>
namespace nm_hdmi {
constexpr uint32_t kPowerCtlBios=0x45400u;
constexpr uint32_t kPowerCtlDriver=0x45404u;
constexpr uint32_t kPW2Index=1u;
constexpr uint32_t kPWBIndex=6u;
constexpr uint32_t kPowerRequest(uint32_t index) {return 1u<<(2*index+1);}
constexpr uint32_t kPowerState(uint32_t index) {return 1u<<(2*index);}
constexpr uint32_t kPW2Both=kPowerRequest(kPW2Index)|kPowerState(kPW2Index);
constexpr uint32_t kPWBBoth=kPowerRequest(kPWBIndex)|kPowerState(kPWBIndex);
enum class MtlPowerStatus { on, pw2NotReady, pwBNotReady, inaccessible };
struct MtlPowerSnapshot {
    uint32_t bios, driver;
    MtlPowerStatus status;
};
template <typename IO>
MtlPowerSnapshot inspectMtlBPower(IO &io) {
    const uint32_t bios=io.read32(kPowerCtlBios);
    const uint32_t drv=io.read32(kPowerCtlDriver);
    if(bios==0xffffffffu || drv==0xffffffffu)
        return {bios,drv,MtlPowerStatus::inaccessible};
    if((drv&kPW2Both)!=kPW2Both)
        return {bios,drv,MtlPowerStatus::pw2NotReady};
    if((drv&kPWBBoth)!=kPWBBoth)
        return {bios,drv,MtlPowerStatus::pwBNotReady};
    return {bios,drv,MtlPowerStatus::on};
}
inline const char *mtlPowerName(MtlPowerStatus s) {
    switch (s) {
    case MtlPowerStatus::on: return "pw2-and-pwb-requested-and-on";
    case MtlPowerStatus::pw2NotReady: return "pw2-driver-request-or-state-missing";
    case MtlPowerStatus::pwBNotReady: return "pwb-driver-request-or-state-missing";
    case MtlPowerStatus::inaccessible: return "power-registers-inaccessible";
    }
    return "unknown";
}
} // namespace nm_hdmi
#endif
