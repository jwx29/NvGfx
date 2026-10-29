// SPDX-License-Identifier: MIT
// Validate MTL PW_2/PW_B request+state masks against real macOS baseline.
#include "hdmi_mtl_power.h"
#include <cassert>
#include <cstdio>
struct Fake {
    uint32_t driver=0xfc0fu,bios=0x5405u;
    uint32_t read32(uint32_t a) const {
        return a==nm_hdmi::kPowerCtlBios ? bios :
               a==nm_hdmi::kPowerCtlDriver ? driver : 0xffffffffu;
    }
};
int main() {
    using namespace nm_hdmi;
    static_assert(kPW2Both==0x000cu && kPWBBoth==0x3000u,
                  "Unexpected Meteor Lake power well masks");
    Fake f;
    auto p=inspectMtlBPower(f);
    assert(p.status==MtlPowerStatus::on && p.bios==0x5405u);
    f.driver &= ~kPowerRequest(kPW2Index);
    assert(inspectMtlBPower(f).status==MtlPowerStatus::pw2NotReady);
    f.driver=0xfc0fu & ~kPowerState(kPWBIndex);
    assert(inspectMtlBPower(f).status==MtlPowerStatus::pwBNotReady);
    f.driver=0xffffffffu;
    assert(inspectMtlBPower(f).status==MtlPowerStatus::inaccessible);
    std::puts("PASS: Meteor Lake PW_2/PW_B request+state masks and baseline");
}
