// SPDX-License-Identifier: MIT
// Meteor Lake HDMI B modesetting prerequisites; READ ONLY.
// Offsets checked against Linux i915 display/intel_display_regs.h.
// This file MUST NOT be used to implement an uncoordinated port/pipe
// enable sequence: i915 power-well, C10 PHY, PLL and GGTT ownership required.
#ifndef NMINTELFB_HDMI_MODESET_PREFLIGHT_H
#define NMINTELFB_HDMI_MODESET_PREFLIGHT_H
#include <stdint.h>
#include <stddef.h>
namespace nm_hdmi {
struct Register {
    const char *name;
    uint32_t address;
};
// Intentionally exclude GMBUS2: even reading it acquires a semaphore.
// Intentionally exclude DP AUX/PHY sideband registers: some are unsafe to
// read with the display power well gated. Everything here is ordinary MMIO.
static constexpr Register kDisplayRegisters[] = {
    {"HSW_PWR_WELL_CTL1_BIOS",  0x45400u},
    {"HSW_PWR_WELL_CTL2_DRV",   0x45404u},
    {"ICL_PWR_WELL_CTL_DDI1",   0x45450u},
    {"ICL_PWR_WELL_CTL_DDI2",   0x45454u},
    {"CDCLK_CTL",               0x46000u},
    {"DDI_CLK_SEL_B",           0x46104u},
    {"TRANS_CLK_SEL_B",         0x46144u},
    {"DDI_CTL_DE_A",            0x64000u},
    {"DDI_CTL_DE_B",            0x64100u},
    {"TRANS_DDI_FUNC_CTL_A",   0x60400u},
    {"TRANS_DDI_FUNC_CTL_B",   0x61400u},
    {"TRANSCONF_A",             0x70008u},
    {"TRANSCONF_B",             0x71008u},
    {"PLANE_CTL_A",             0x70180u},
    {"PLANE_CTL_B",             0x71180u},
    {"PLANE_SURF_A",            0x7019cu},
    {"PLANE_SURF_B",            0x7119cu},
    {"HTOTAL_A",                0x60000u},
    {"HTOTAL_B",                0x61000u},
    {"HSYNC_B",                 0x61008u},
    {"VTOTAL_B",                0x6100cu},
    {"VSYNC_B",                 0x61014u}
};
constexpr size_t kRegisterCount = sizeof(kDisplayRegisters) / sizeof(kDisplayRegisters[0]);
constexpr uint32_t kPreflightABI = 0x48445031u; // 'HDP1'
constexpr uint32_t kRegEnable = 1u << 31;
struct PreflightSnapshot {
    uint32_t abi;
    uint32_t count;
    uint32_t offsets[kRegisterCount];
    uint32_t values[kRegisterCount];
};
static_assert(sizeof(PreflightSnapshot) == (2 + 2 * kRegisterCount) * 4,
              "preflight snapshot must be plain aligned uint32 words");

template <typename IO> void capture(IO &io, PreflightSnapshot &snapshot) {
    snapshot.abi = kPreflightABI;
    snapshot.count = uint32_t(kRegisterCount);
    for (size_t i = 0; i < kRegisterCount; ++i) {
        snapshot.offsets[i] = kDisplayRegisters[i].address;
        snapshot.values[i] = io.read32(kDisplayRegisters[i].address);
    }
}
inline uint32_t valueAt(const PreflightSnapshot &s, uint32_t address) {
    for (size_t i=0;i<kRegisterCount;++i)
        if (s.offsets[i]==address) return s.values[i];
    return 0xffffffffu;
}
enum class PreflightReason {
    ok,
    inaccessible,
    panel_off,
    hdmi_pipe_in_use,
    hdmi_port_in_use
};
inline const char *reasonText(PreflightReason r) {
    switch(r) {
    case PreflightReason::ok: return "pipe-b-unowned-ready-for-planning";
    case PreflightReason::inaccessible: return "inaccessible-display-mmio";
    case PreflightReason::panel_off: return "firmware-edp-not-active";
    case PreflightReason::hdmi_pipe_in_use: return "pipe-b-already-active-or-owned";
    case PreflightReason::hdmi_port_in_use: return "ddi-b-already-active-or-owned";
    }
    return "unknown";
}
inline PreflightReason validateIdleDisplay(const PreflightSnapshot &s) {
    if (s.abi!=kPreflightABI || s.count!=kRegisterCount)
        return PreflightReason::inaccessible;
    for(size_t i=0;i<kRegisterCount;++i)
        if (s.values[i]==0xffffffffu)
            return PreflightReason::inaccessible;
    if (!(valueAt(s,0x70008u)&kRegEnable) ||
        !(valueAt(s,0x70180u)&kRegEnable))
        return PreflightReason::panel_off;
    if ((valueAt(s,0x71008u)&kRegEnable) ||
        (valueAt(s,0x71180u)&kRegEnable) ||
        (valueAt(s,0x61400u)&kRegEnable))
        return PreflightReason::hdmi_pipe_in_use;
    if (valueAt(s,0x64100u)&kRegEnable)
        return PreflightReason::hdmi_port_in_use;
    return PreflightReason::ok;
}
} // namespace nm_hdmi
#endif
