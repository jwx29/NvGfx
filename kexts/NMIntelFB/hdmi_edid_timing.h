// SPDX-License-Identifier: MIT
// Portable EDID preferred detailed-timing parser. No hardware writes.
// Returned timing is a candidate only; does not allocate any scanout head.
#ifndef NMINTELFB_HDMI_EDID_TIMING_H
#define NMINTELFB_HDMI_EDID_TIMING_H
#include <stdint.h>
#include <stddef.h>
namespace nm_hdmi {
struct Timing {
    uint32_t pixelClockHz;
    uint32_t hActive, hSyncStart, hSyncEnd, hTotal;
    uint32_t vActive, vSyncStart, vSyncEnd, vTotal;
    uint32_t edidSyncFlags;
};
static_assert(sizeof(Timing) == 40, "Timing must be 10 packed uint32 fields");
inline bool parsePreferredTiming(const uint8_t *edid, size_t length,
                                Timing &t) {
    if (!edid || length < 128) return false;
    static const uint8_t sig[8] = {0,255,255,255,255,255,255,0};
    for (unsigned i=0;i<8;++i) if (edid[i]!=sig[i]) return false;
    unsigned checksum=0;
    for (unsigned i=0;i<128;++i) checksum+=edid[i];
    if (checksum & 255u) return false;
    // The first EDID detailed timing descriptor is at byte 54.
    const uint8_t *d=edid+54;
    const uint32_t clock=uint32_t(d[0] | (d[1]<<8))*10000u;
    if (clock < 25000000u || clock > 600000000u) return false;
    const uint32_t ha=uint32_t(d[2] | ((d[4] & 0xF0u)<<4));
    const uint32_t hb=uint32_t(d[3] | ((d[4] & 0x0Fu)<<8));
    const uint32_t va=uint32_t(d[5] | ((d[7] & 0xF0u)<<4));
    const uint32_t vb=uint32_t(d[6] | ((d[7] & 0x0Fu)<<8));
    const uint32_t hso=uint32_t(d[8] | ((d[11] & 0xC0u)<<2));
    const uint32_t hsw=uint32_t(d[9] | ((d[11] & 0x30u)<<4));
    const uint32_t vso=uint32_t((d[10]>>4) | ((d[11] & 0x0Cu)<<2));
    const uint32_t vsw=uint32_t((d[10]&15u) | ((d[11] & 0x03u)<<4));
    // EDID 1.3 digital separate sync only. Other timing/sync formats
    // require a proper modeset translation and are not guessed here.
    if ((d[17]&0x18u)!=0x18u || ha<320 || va<200 ||
        hb<8 || vb<2 || hsw==0 || vsw==0) return false;
    const uint32_t ht=ha+hb, vt=va+vb;
    if (ht>8192u || vt>8192u ||
        hso+hsw>hb || vso+vsw>vb) return false;
    t={clock, ha, ha+hso, ha+hso+hsw, ht,
       va, va+vso, va+vso+vsw, vt, d[17]};
    return true;
}
} // namespace nm_hdmi
#endif
