// SPDX-License-Identifier: MIT
// Simulated MTL power-well lease tests; never touches real MMIO.
#include "hdmi_mtl_power_lease.h"
#include <cassert>
#include <cstdio>
#include <vector>
struct FakePower {
    uint32_t bios = 0x5405u, driver = 0xfc0fu;
    bool autoPower = true, pwBStuck = false, inaccessible = false, ignoreRelease = false;
    unsigned waits = 0;
    std::vector<uint32_t> writes;
    uint32_t read32(uint32_t reg) const {
        if (inaccessible) return 0xffffffffu;
        return reg == nm_hdmi::kPowerCtlBios ? bios :
               reg == nm_hdmi::kPowerCtlDriver ? driver : 0xffffffffu;
    }
    void write32(uint32_t reg, uint32_t val) {
        assert(reg == nm_hdmi::kPowerCtlDriver);
        writes.push_back(val);
        if (ignoreRelease && (val & nm_hdmi::kMtlBPowerRequestMask) == 0)
            return;
        // Hardware reports well state after its corresponding driver request.
        driver = val;
        if (autoPower) {
            if (driver & nm_hdmi::kPowerRequest(nm_hdmi::kPW2Index))
                driver |= nm_hdmi::kPowerState(nm_hdmi::kPW2Index);
            else driver &= ~nm_hdmi::kPowerState(nm_hdmi::kPW2Index);
            if ((driver & nm_hdmi::kPowerRequest(nm_hdmi::kPWBIndex)) && !pwBStuck)
                driver |= nm_hdmi::kPowerState(nm_hdmi::kPWBIndex);
            else driver &= ~nm_hdmi::kPowerState(nm_hdmi::kPWBIndex);
        }
    }
    void delayUs(unsigned us) { assert(us==50u); ++waits; }
};
int main() {
    using namespace nm_hdmi;
    static_assert(kMtlBPowerRequestMask == 0x2008u, "MTL request bits");
    {
        FakePower io; MtlBPowerLease lease;
        assert(acquireMtlBPower(io,lease)==MtlBPowerLeaseResult::ok);
        assert(lease.active && lease.ownedRequests==0);
        assert(io.writes.empty()); // real hardware baseline: must not write
        assert(acquireMtlBPower(io,lease)==MtlBPowerLeaseResult::alreadyAcquired);
        assert(releaseMtlBPower(io,lease)==MtlBPowerLeaseResult::ok);
        assert(io.driver==0xfc0fu && io.writes.empty());
    }
    {
        FakePower io; io.driver &= ~kMtlBPowerRequestMask;
        io.driver &= ~(kPowerState(kPW2Index)|kPowerState(kPWBIndex));
        uint32_t initial = io.driver;
        MtlBPowerLease lease;
        assert(acquireMtlBPower(io,lease)==MtlBPowerLeaseResult::ok);
        assert(lease.active && lease.ownedRequests==kMtlBPowerRequestMask);
        assert(io.writes.size()==2);
        assert((io.writes[0] & kPowerRequest(kPW2Index))!=0);
        assert((io.writes[0] & kPowerRequest(kPWBIndex))==0);
        assert((io.writes[1] & kPowerRequest(kPWBIndex))!=0);
        io.driver |= 0x10000u; // unrelated external request made during lease
        assert(releaseMtlBPower(io,lease)==MtlBPowerLeaseResult::ok);
        assert(io.driver == (initial | 0x10000u));
    }
    {
        FakePower io; io.driver &= ~kPowerRequest(kPWBIndex);
        io.driver &= ~kPowerState(kPWBIndex);
        MtlBPowerLease lease;
        assert(acquireMtlBPower(io,lease)==MtlBPowerLeaseResult::ok);
        assert(lease.ownedRequests==kPowerRequest(kPWBIndex));
        assert(releaseMtlBPower(io,lease)==MtlBPowerLeaseResult::ok);
        assert(io.driver & kPW2Both); // borrowed PW2 must remain on
        assert(!(io.driver & kPowerRequest(kPWBIndex)));
    }
    {
        FakePower io; io.driver &= ~kMtlBPowerRequestMask;
        io.driver &= ~(kPowerState(kPW2Index)|kPowerState(kPWBIndex));
        io.autoPower=false;
        MtlBPowerLease lease;
        assert(acquireMtlBPower(io,lease,3)==MtlBPowerLeaseResult::timeout);
        assert(!lease.active && !lease.ownedRequests);
        assert(!(io.driver & kMtlBPowerRequestMask));
        assert(io.waits==3);
    }
    {
        FakePower io; io.driver &= ~kMtlBPowerRequestMask;
        io.driver &= ~(kPowerState(kPW2Index)|kPowerState(kPWBIndex));
        io.pwBStuck = true;
        MtlBPowerLease lease;
        assert(acquireMtlBPower(io,lease,3)==MtlBPowerLeaseResult::timeout);
        assert(!lease.active && !lease.ownedRequests);
        assert(!(io.driver & kMtlBPowerRequestMask));
    }
    {
        FakePower io; io.inaccessible=true;
        MtlBPowerLease lease;
        assert(acquireMtlBPower(io,lease)==MtlBPowerLeaseResult::inaccessible);
        assert(io.writes.empty());
    }
    {
        FakePower io; io.driver &= ~kMtlBPowerRequestMask;
        io.driver &= ~(kPowerState(kPW2Index)|kPowerState(kPWBIndex));
        io.autoPower=false; io.ignoreRelease=true;
        MtlBPowerLease lease;
        assert(acquireMtlBPower(io,lease,1)==MtlBPowerLeaseResult::rollbackFailed);
        assert(lease.ownedRequests==kPowerRequest(kPW2Index)); // don't hide failed cleanup
    }
    std::puts("PASS: MTL PW_2/PW_B request ordering, borrowed wells, timeouts and isolated rollback");
}
