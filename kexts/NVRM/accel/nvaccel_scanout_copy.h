/*
 * XeGfx-only scanout row-copy primitive.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Normal mode preserves the existing volatile dword loads/stores. Bulk mode is an
 * explicit experiment for a CPU-mapped framebuffer backed by ordinary memory.
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
            // Opt-in only: memcpy allows the compiler/kernel libc to use a
            // wider copy than millions of individual volatile 32-bit stores.
            memcpy(const_cast<uint32_t *>(d),
                   const_cast<const uint32_t *>(s),
                   (size_t)copyWords * sizeof(uint32_t));
        } else {
            for (unsigned x = 0; x < copyWords; ++x) d[x] = s[x];
        }
    }
}
