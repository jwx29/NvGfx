// SPDX-License-Identifier: MIT
// Bounded Meteor Lake GMBUS EDID probe (no pipe/PLL/PHY/power-well writes).
// Ported from Intel i915 intel_gmbus.c and intel_gmbus_regs.h algorithms.
// Transaction is ONLY invoked explicitly by privileged NMIntelFB::setProperties.
// The IO implementation must provide read32/write32/delayUs.
#ifndef NMINTELFB_HDMI_GMBUS_H
#define NMINTELFB_HDMI_GMBUS_H
#include <stdint.h>
#include <stddef.h>
namespace nm_hdmi {
constexpr uint32_t kGMBUS0 = 0xC5100u;
constexpr uint32_t kGMBUS1 = 0xC5104u;
constexpr uint32_t kGMBUS2 = 0xC5108u;
constexpr uint32_t kGMBUS3 = 0xC510Cu;
constexpr uint32_t kDPBPin = 2u; // Lenovo EFP1, VBT BDB child DDC pin 0x02
constexpr uint32_t kSWReady = 1u << 30;
constexpr uint32_t kCycleWait = 1u << 25;
constexpr uint32_t kCycleIndex = 2u << 25;
constexpr uint32_t kCycleStop = 4u << 25;
constexpr uint32_t kReady = 1u << 11;
constexpr uint32_t kNAK = 1u << 10;
constexpr uint32_t kStallTimeout = 1u << 13;
constexpr uint32_t kActive = 1u << 9;
constexpr uint32_t kInUse = 1u << 15;
constexpr uint32_t kWaitPhase = 1u << 14;
constexpr uint32_t kClearError = 1u << 31;
enum class Status { ok, busy, inaccessible, nak, timeout, idle_timeout, header, checksum };
inline const char *statusName(Status x) {
    switch (x) {
    case Status::ok: return "ok";
    case Status::busy: return "busy";
    case Status::inaccessible: return "inaccessible";
    case Status::nak: return "nak";
    case Status::timeout: return "timeout";
    case Status::idle_timeout: return "idle-timeout";
    case Status::header: return "bad-header";
    case Status::checksum: return "bad-checksum";
    }
    return "unknown";
}
inline bool validHeader(const uint8_t *b) {
    static const uint8_t sig[] = { 0, 255, 255, 255, 255, 255, 255, 0 };
    for (unsigned i=0; i<sizeof(sig); ++i) if (b[i]!=sig[i]) return false;
    return true;
}
inline bool validChecksum(const uint8_t *b) {
    unsigned sum=0;
    for (unsigned i=0; i<128; ++i) sum += b[i];
    return (sum & 255u)==0;
}
template <typename IO> Status readBlock(IO &io, uint8_t offset, uint8_t *dst,
                                        uint32_t &lastStatus) {
    // Intel GMBUS2.INUSE is a READ-TO-ACQUIRE semaphore (not an
    // ordinary status bit). The FIRST read returning 0 acquires ownership;
    // subsequent status reads report INUSE=1 even for the SAME owner.
    // Never read GMBUS2 speculatively, never mistake subsequent INUSE=1
    // for a second owner, and release by writing ONE to GMBUS2.INUSE.
    const uint32_t old0 = io.read32(kGMBUS0);
    if (old0 == 0xffffffffu) {
        lastStatus = old0;
        return Status::inaccessible;
    }
    lastStatus = io.read32(kGMBUS2); // exactly one acquisition read
    if (lastStatus == 0xffffffffu)
        return Status::inaccessible;
    if (lastStatus & kInUse)
        return Status::busy; // another owner; MUST NOT release its lock
    // We own the software semaphore now. An existing selection or active
    // hardware transfer is not ours: release only our semaphore, no reset.
    if (old0 != 0 || (lastStatus & kActive)) {
        io.write32(kGMBUS2, kInUse);
        return Status::busy;
    }
    Status result=Status::ok;
    io.write32(kGMBUS0, kDPBPin); // 100 kHz, no unrelated DDI or pipe changes
    // Single-byte index read: slave 0x50, 128 bytes, INDEX cycle.
    io.write32(kGMBUS1, kSWReady | kCycleIndex | kCycleWait | (128u << 16) |
                           (uint32_t(offset) << 8) | (0x50u << 1) | 1u);
    for (unsigned word=0; word<32; ++word) {
        bool ready=false;
        // <=20 ms per dword, bounded even with no connected monitor.
        for (unsigned poll=0; poll<400; ++poll) {
            lastStatus=io.read32(kGMBUS2);
            if (lastStatus == 0xffffffffu) { result=Status::inaccessible; break; }
            if (lastStatus & kNAK) { result=Status::nak; break; }
            if (lastStatus & kStallTimeout) { result=Status::timeout; break; }
            if (lastStatus & kReady) { ready=true; break; }
            io.delayUs(50);
        }
        if (result!=Status::ok) break;
        if (!ready) { result=Status::timeout; break; }
        const uint32_t value=io.read32(kGMBUS3);
        for (unsigned j=0; j<4; ++j)
            dst[word*4+j]=uint8_t(value >> (8*j));
    }
    if (result==Status::ok) {
        // Intel i915 waits for the WAIT phase before the STOP cycle.
        bool waiting=false;
        for (unsigned poll=0; poll<400; ++poll) {
            lastStatus=io.read32(kGMBUS2);
            if (lastStatus & kNAK) { result=Status::nak; break; }
            if (lastStatus & kStallTimeout) { result=Status::timeout; break; }
            if (lastStatus & kWaitPhase) { waiting=true; break; }
            io.delayUs(50);
        }
        if (result==Status::ok && !waiting) result=Status::timeout;
    }
    if (result==Status::ok) {
        io.write32(kGMBUS1, kCycleStop | kSWReady);
        bool idle=false;
        for (unsigned poll=0; poll<200; ++poll) {
            lastStatus=io.read32(kGMBUS2);
            if (!(lastStatus & kActive)) { idle=true; break; }
            io.delayUs(50);
        }
        if (!idle) result=Status::idle_timeout;
    }
    if (result!=Status::ok) {
        // Reset GMBUS command/error only. Never touch display pipes or PLLs.
        io.write32(kGMBUS1, kClearError);
        io.write32(kGMBUS1, 0);
    }
    io.write32(kGMBUS0, 0);
    // Every successful semaphore acquisition MUST be paired with release,
    // including NAK, timeout and MMIO read failure inside the transfer.
    io.write32(kGMBUS2, kInUse);
    return result;
}
template <typename IO> Status readEDID(IO &io, uint8_t *out, size_t &length,
                                      uint32_t &lastStatus) {
    length=0;
    Status s=readBlock(io, 0, out, lastStatus);
    if (s!=Status::ok) return s;
    if (!validHeader(out)) return Status::header;
    if (!validChecksum(out)) return Status::checksum;
    length=128;
    // First extension only: 256-byte ceiling and no E-DDC segment writes.
    if (!out[126]) return Status::ok;
    s=readBlock(io, 128, out+128, lastStatus);
    if (s!=Status::ok) return s;
    if (!validChecksum(out+128)) return Status::checksum;
    length=256;
    return Status::ok;
}
} // namespace nm_hdmi
#endif
