// Host-side correctness test for the exact copy helper used in XeGfxAccel.
// clang++ -std=c++17 -Wall -Wextra -Werror tools/test_xegfx_scanout_copy.cpp -o /tmp/test_xegfx_scanout_copy
#include "../kexts/NVRM/accel/nvaccel_scanout_copy.h"
#include <cassert>
#include <cstdio>
#include <vector>
static void check(unsigned rows, unsigned srcStride, unsigned dstStride, unsigned words) {
    std::vector<uint32_t> src((size_t)rows*srcStride + 4, 0xABCD4321u);
    std::vector<uint32_t> result((size_t)rows*dstStride + 4, 0xEEAAEEAAu);
    std::vector<uint32_t> ref = result;
    for (unsigned y=0; y<rows; ++y)
        for (unsigned x=0; x<words; ++x)
            src[(size_t)y*srcStride + x] = 0x12340000u + y*997u+x;
    nvAccelScanoutCopyRows(ref.data(), src.data(), rows, dstStride, srcStride, words, false);
    nvAccelScanoutCopyRows(result.data(), src.data(), rows, dstStride, srcStride, words, true);
    assert(ref == result);
    for (unsigned y=0; y<rows; ++y)
        for (unsigned x=0; x<dstStride; ++x)
            assert(result[(size_t)y*dstStride+x] == (x<words ? src[(size_t)y*srcStride+x] : 0xEEAAEEAAu));
}
int main() {
    for (unsigned rows : {0u, 1u, 2u, 7u, 19u})
        for (unsigned words : {0u, 1u, 3u, 7u, 8u, 64u, 257u})
            check(rows, words+6, words+9, words);
    std::vector<uint32_t> same(512, 0x12345678u), prior = same;
    nvAccelScanoutCopyRows(same.data(), same.data(), 8, 64, 64, 64, false);
    nvAccelScanoutCopyRows(same.data(), same.data(), 8, 64, 64, 64, true);
    assert(same == prior);
    puts("XeGfx scanout copy: scalar and memcpy outputs match, padding and self-copy checks passed");
}
