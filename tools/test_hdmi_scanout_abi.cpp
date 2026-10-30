// SPDX-License-Identifier: MIT
// Portable ABI/layout validation for the Xen i915 <-> XNU HDMI scanout bridge.
// No GPU operations are performed by this test.
#include "xg_i915.h"
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <type_traits>
int main() {
    static_assert(sizeof(xg_i915_hdmi_surface)==48, "C ABI must be 48 bytes");
    static_assert(alignof(xg_i915_hdmi_surface)==8, "C ABI must align uint64");
    static_assert(offsetof(xg_i915_hdmi_surface,bytes)==24, "wrong bytes offset");
    static_assert(offsetof(xg_i915_hdmi_surface,ggtt_offset)==32, "wrong GGTT offset");
    static_assert(offsetof(xg_i915_hdmi_surface,kva)==40, "wrong KVA offset");
    static_assert(std::is_standard_layout<xg_i915_hdmi_surface>::value,"ABI must be POD");
    xg_i915_hdmi_surface r={};
    r.abi=XG_HDMI_SURFACE_ABI;
    r.op=XG_HDMI_SURFACE_ALLOC;
    r.width=1920u;
    r.height=1080u;
    r.pitch=7680u;
    r.bytes=1920ull*1080ull*4ull;
    assert(r.bytes==8294400ull && r.pitch*r.height==r.bytes);
    assert(XG_HDMI_SURFACE_ALLOC != XG_HDMI_SURFACE_RELEASE &&
           XG_HDMI_SURFACE_QUERY != XG_HDMI_SURFACE_ALLOC);
    std::puts("PASS: XeGfx i915 HDMI GGTT scanout ABI and 1080p buffer geometry");
}
