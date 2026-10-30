/*
 * Opt-in WindowServer IOSurface staging helper.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * CPU-only pool; never reuses a lease while its GPU readback / publication is pending.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define NVMTL_WS_STAGE_SLOTS 3u
#define NVMTL_WS_STAGE_LIMIT (16u << 20)
typedef struct {
    pthread_mutex_t mutex;
    void *bytes;
    size_t capacity;
} nvmtl_ws_stage_slot;
static nvmtl_ws_stage_slot nvmtl_ws_stage_slots[NVMTL_WS_STAGE_SLOTS] = {
    { PTHREAD_MUTEX_INITIALIZER, NULL, 0 },
    { PTHREAD_MUTEX_INITIALIZER, NULL, 0 },
    { PTHREAD_MUTEX_INITIALIZER, NULL, 0 }
};
static int nvmtl_ws_stage_plan(size_t width, size_t height, size_t bpp,
                               size_t *packed, size_t *total)
{
    if (!width || !height || !bpp || !packed || !total || width > SIZE_MAX / bpp)
        return -1;
    const size_t row = width * bpp;
    if (height > SIZE_MAX / row || height * row > NVMTL_WS_STAGE_LIMIT) return -1;
    *packed = row;
    *total = height * row;
    return 0;
}
static int nvmtl_ws_stage_acquire(size_t total, void **out)
{
    if (!out || !total || total > NVMTL_WS_STAGE_LIMIT) return -1;
    for (unsigned i = 0; i < NVMTL_WS_STAGE_SLOTS; ++i) {
        nvmtl_ws_stage_slot *s = &nvmtl_ws_stage_slots[i];
        if (pthread_mutex_trylock(&s->mutex)) continue;
        if (s->capacity < total) {
            void *p = realloc(s->bytes, total);
            if (!p) { pthread_mutex_unlock(&s->mutex); continue; }
            s->bytes = p;
            s->capacity = total;
        }
        *out = s->bytes;
        return (int)i;
    }
    return -1;
}
static void nvmtl_ws_stage_release(int lease)
{
    if (lease >= 0 && lease < (int)NVMTL_WS_STAGE_SLOTS)
        pthread_mutex_unlock(&nvmtl_ws_stage_slots[lease].mutex);
}
/* dstAvailable begins at the actual plane address, not the start of IOSurface. */
static int nvmtl_ws_stage_copy(void *dst, size_t dstPitch, size_t dstAvailable,
                               const void *src, size_t packed, size_t rows)
{
    if (!dst || !src || !packed || !rows || dstPitch < packed ||
        rows > SIZE_MAX / packed ||
        (rows - 1) > (SIZE_MAX - packed) / dstPitch)
        return -1;
    const size_t needed = (rows - 1) * dstPitch + packed;
    if (needed > dstAvailable) return -1;
    if (dstPitch == packed) memcpy(dst, src, rows * packed);
    else for (size_t y = 0; y < rows; ++y)
        memcpy((uint8_t *)dst + y * dstPitch, (const uint8_t *)src + y * packed, packed);
    return 0;
}
