// SPDX-License-Identifier: MIT
// Test raw MMIO capture and admission without accessing GPU hardware.
#include "hdmi_modeset_preflight.h"
#include <cassert>
#include <cstdio>
struct Fake {
    uint32_t a[16384]={}; // MMIO is handled as sparse address comparisons
    bool panel=true, port=false, pipe=false, inaccessible=false;
    uint32_t read32(uint32_t off) const {
        if (inaccessible && off==0x64100u) return 0xffffffffu;
        if (off==0x70008u || off==0x70180u)
            return panel ? (1u<<31) : 0u;
        if (off==0x71008u || off==0x71180u || off==0x61400u)
            return pipe ? (1u<<31) : 0u;
        if (off==0x64100u) return port ? (1u<<31) : 0u;
        return 0u;
    }
};
int main() {
    using namespace nm_hdmi;
    static_assert(kRegisterCount==22, "Unexpected register snapshot ABI");
    for (auto &r : kDisplayRegisters)
        assert(r.address!=0xC5108u && "GMBUS2 is read-to-acquire");
    Fake io;PreflightSnapshot s={};
    capture(io,s);
    assert(s.abi==kPreflightABI && s.count==kRegisterCount);
    assert(validateIdleDisplay(s)==PreflightReason::ok);
    assert(valueAt(s,0x70008u)==0x80000000u);
    io.panel=false;capture(io,s);
    assert(validateIdleDisplay(s)==PreflightReason::panel_off);
    io.panel=true;io.pipe=true;capture(io,s);
    assert(validateIdleDisplay(s)==PreflightReason::hdmi_pipe_in_use);
    io.pipe=false;io.port=true;capture(io,s);
    assert(validateIdleDisplay(s)==PreflightReason::hdmi_port_in_use);
    io.port=false;io.inaccessible=true;capture(io,s);
    assert(validateIdleDisplay(s)==PreflightReason::inaccessible);
    std::puts("PASS: HDMI modeset MMIO snapshot + eDP/pipe/port ownership checks");
}
