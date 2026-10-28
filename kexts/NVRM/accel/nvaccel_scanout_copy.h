/*
 * XeGfx-only scanout row-copy primitive.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Scalar mode preserves volatile dword loads/stores. Bulk mode uses x86
 * REP MOVSB for large linear scanout transfers without using SIMD registers.
 * Never use this on GPU MMIO registers, and don't enable bulk mode until checked
 * on the actual Intel scanout mapping. Row strides and padding are preserved.
 */
#pragma once
#ifdef KERNEL
#include <libkern/libkern.h>  // kernel memcpy / integer types (-nostdinc)
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#endif

static inline void nvAccelScanoutCopyRows(volatile uint32_t *dst,
                                          const volatile uint32_t *src,
                                          unsigned rows,
                                          unsigned dstStrideWords,
                                          unsigned srcStrideWords,
                                          unsigned copyWords,
                                          bool bulk)
{
    if (!src || !dst || !rows || !copyWords) return;
    for (unsigned y = 0; y < rows; ++y) {
        volatile uint32_t *d = dst + (size_t)y * dstStrideWords;
        const volatile uint32_t *s = src + (size_t)y * srcStrideWords;
        // WindowServer can hand us the scanout as its own source. Re-reading
        // write-combined framebuffer memory can be very slow and changes nothing.
        if (d == s) continue;
        if (bulk) {
#if defined(__x86_64__)
            // REP MOVSB is the integer-only x86 copy primitive used by
            // VedCloud's Meteor Lake path. Preserve the volatile/IO mapping
            // semantics and don't use kernel SIMD/FPU state.
            volatile unsigned char *to = (volatile unsigned char *)d;
            const volatile unsigned char *from = (const volatile unsigned char *)s;
            size_t count = (size_t)copyWords * sizeof(uint32_t);
            __asm__ __volatile__("rep movsb" : "+D"(to), "+S"(from), "+c"(count) :: "memory");
#else
            memcpy(const_cast<uint32_t *>(d), const_cast<const uint32_t *>(s),
                   (size_t)copyWords * sizeof(uint32_t));
#endif
        } else {
            for (unsigned x = 0; x < copyWords; ++x) d[x] = s[x];
        }
    }
#if defined(__x86_64__)
    // Order write-combined framebuffer stores before the caller publishes
    // completion; this is intentionally outside the per-row loop.
    if (bulk) __asm__ __volatile__("sfence" ::: "memory");
#endif
}
