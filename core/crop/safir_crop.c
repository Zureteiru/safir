#include "safir_crop.h"
#include <string.h>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#include <intrin.h>
#endif
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

bool safir_crop_auto_invert(uint8_t *gray, uint16_t w, uint16_t h) {
    if (!gray || w == 0 || h == 0) {
        return false;
    }

    uint64_t border_sum = 0;
    uint32_t count = 0;

    /* Top and bottom rows */
    for (uint16_t x = 0; x < w; ++x) {
        border_sum += gray[x];
        count++;
        if (h > 1) {
            border_sum += gray[(uint32_t)(h - 1) * (uint32_t)w + x];
            count++;
        }
    }

    /* Left and right columns (excluding corners already counted) */
    for (uint16_t y = 1; y + 1 < h; ++y) {
        border_sum += gray[(uint32_t)y * (uint32_t)w];
        count++;
        if (w > 1) {
            border_sum += gray[(uint32_t)y * (uint32_t)w + (w - 1)];
            count++;
        }
    }

    if (count == 0) {
        return false;
    }

    uint32_t avg = (uint32_t)(border_sum / count);
    if (avg < 128) {
        /* Border is dark -> light text on dark background.
         * Invert so glyphs are dark-on-light for OCR recognizers. */
        size_t total_pixels = (size_t)w * (size_t)h;
        for (size_t i = 0; i < total_pixels; ++i) {
            gray[i] = (uint8_t)(255 - gray[i]);
        }
        return true;
    }

    return false;
}

bool safir_crop_rgba_to_gray(const uint8_t *rgba_1280x720, SafirRect region, uint8_t *out_gray) {
    if (!rgba_1280x720 || !out_gray) {
        return false;
    }
    if (region.w == 0 || region.h == 0) {
        return false;
    }
    if (region.x >= SAFIR_SCREEN_WIDTH || region.y >= SAFIR_SCREEN_HEIGHT) {
        return false;
    }

    uint16_t crop_w = region.w;
    uint16_t crop_h = region.h;

    if ((uint32_t)region.x + (uint32_t)crop_w > SAFIR_SCREEN_WIDTH) {
        crop_w = (uint16_t)(SAFIR_SCREEN_WIDTH - region.x);
    }
    if ((uint32_t)region.y + (uint32_t)crop_h > SAFIR_SCREEN_HEIGHT) {
        crop_h = (uint16_t)(SAFIR_SCREEN_HEIGHT - region.y);
    }


    /* Convert RGBA8888 crop to 8-bit luminance: Y = (77*R + 150*G + 29*B) >> 8 */
    for (uint16_t ry = 0; ry < crop_h; ++ry) {
        uint32_t screen_y = (uint32_t)region.y + ry;
        const uint8_t *src_row = rgba_1280x720 + ((screen_y * SAFIR_SCREEN_WIDTH + (uint32_t)region.x) * 4U);
        uint8_t *dst_row = out_gray + ((uint32_t)ry * (uint32_t)region.w);

#if defined(__aarch64__)
        uint16_t rx = 0;
        for (; rx + 8 <= crop_w; rx += 8) {
            uint8x8x4_t rgba = vld4_u8(src_row + rx * 4U);
            uint16x8_t r16 = vmull_u8(rgba.val[0], vdup_n_u8(77));
            uint16x8_t g16 = vmlal_u8(r16, rgba.val[1], vdup_n_u8(150));
            uint16x8_t b16 = vmlal_u8(g16, rgba.val[2], vdup_n_u8(29));
            uint8x8_t y8 = vshrn_n_u16(b16, 8);
            vst1_u8(dst_row + rx, y8);
        }
        for (; rx < crop_w; ++rx) {
            uint32_t r = src_row[rx * 4U + 0];
            uint32_t g = src_row[rx * 4U + 1];
            uint32_t b = src_row[rx * 4U + 2];
            dst_row[rx] = (uint8_t)((77U * r + 150U * g + 29U * b) >> 8);
        }
#else
        /* SWAR 64-bit word loads (2 RGBA pixels per 64-bit load) */
        uint16_t rx = 0;
        for (; rx + 2 <= crop_w; rx += 2) {
            uint64_t p;
            memcpy(&p, src_row + rx * 4U, sizeof(uint64_t));
            uint32_t r0 = (uint32_t)(p & 0xFFULL);
            uint32_t g0 = (uint32_t)((p >> 8) & 0xFFULL);
            uint32_t b0 = (uint32_t)((p >> 16) & 0xFFULL);
            dst_row[rx + 0] = (uint8_t)((77U * r0 + 150U * g0 + 29U * b0) >> 8);

            uint32_t r1 = (uint32_t)((p >> 32) & 0xFFULL);
            uint32_t g1 = (uint32_t)((p >> 40) & 0xFFULL);
            uint32_t b1 = (uint32_t)((p >> 48) & 0xFFULL);
            dst_row[rx + 1] = (uint8_t)((77U * r1 + 150U * g1 + 29U * b1) >> 8);
        }
        for (; rx < crop_w; ++rx) {
            uint32_t r = src_row[rx * 4U + 0];
            uint32_t g = src_row[rx * 4U + 1];
            uint32_t b = src_row[rx * 4U + 2];
            dst_row[rx] = (uint8_t)((77U * r + 150U * g + 29U * b) >> 8);
        }
#endif

        /* If crop was clamped horizontally, zero remaining pixels in destination row */
        if (crop_w < region.w) {
            memset(dst_row + crop_w, 0, (size_t)(region.w - crop_w));
        }
    }

    /* If crop was clamped vertically, zero remaining rows */
    if (crop_h < region.h) {
        for (uint16_t ry = crop_h; ry < region.h; ++ry) {
            uint8_t *dst_row = out_gray + ((uint32_t)ry * (uint32_t)region.w);
            memset(dst_row, 0, region.w);
        }
    }

    /* Polarity auto-inversion */
    safir_crop_auto_invert(out_gray, region.w, region.h);

    return true;
}

uint8_t *safir_crop_rgba_to_scratch(const uint8_t *rgba_1280x720, SafirRect region) {
    if (!rgba_1280x720 || region.w == 0 || region.h == 0) {
        return NULL;
    }
    size_t full_bytes = (size_t)region.w * (size_t)region.h;
    if (full_bytes <= SAFIR_POOL_OCR_SCRATCH_MAX_BYTES) {
        uint8_t *buf = (uint8_t *)safir_alloc(POOL_OCR_SCRATCH, full_bytes);
        if (!buf) {
            return NULL;
        }
        if (!safir_crop_rgba_to_gray(rgba_1280x720, region, buf)) {
            return NULL;
        }
        return buf;
    }

    /* 2x Downsampling fallback for full dialogue regions (550x105 = 57,750 B) */
    uint16_t dw = (region.w + 1) / 2;
    uint16_t dh = (region.h + 1) / 2;
    size_t down_bytes = (size_t)dw * (size_t)dh;
    uint8_t *buf = (uint8_t *)safir_alloc(POOL_OCR_SCRATCH, down_bytes);
    if (!buf) {
        return NULL;
    }

    for (uint16_t y = 0; y < dh; ++y) {
        uint32_t sy = (uint32_t)region.y + y * 2;
        if (sy >= SAFIR_SCREEN_HEIGHT) break;
        const uint8_t *src_row = rgba_1280x720 + ((sy * SAFIR_SCREEN_WIDTH + (uint32_t)region.x) * 4U);
        uint8_t *dst_row = buf + (size_t)y * dw;
        for (uint16_t x = 0; x < dw; ++x) {
            uint32_t sx = x * 2;
            if ((uint32_t)region.x + sx >= SAFIR_SCREEN_WIDTH) break;
            uint32_t r = src_row[sx * 4U + 0];
            uint32_t g = src_row[sx * 4U + 1];
            uint32_t b = src_row[sx * 4U + 2];
            dst_row[x] = (uint8_t)((77U * r + 150U * g + 29U * b) >> 8);
        }
    }
    safir_crop_auto_invert(buf, dw, dh);
    return buf;
}

uint64_t safir_compute_dhash(const uint8_t *gray, uint16_t w, uint16_t h) {
    if (!gray || w < 9 || h < 8) {
        return 0;
    }

    uint8_t sampled[8][9];

    /* Precompute grid boundary intervals with fixed-point arithmetic (zero divisions in loop) */
    uint32_t y_bounds[9];
    for (uint32_t gy = 0; gy < 9; ++gy) {
        y_bounds[gy] = (gy * (uint32_t)h) >> 3; /* Bitshift >> 3 replaces / 8 */
    }
    uint32_t x_bounds[10];
    for (uint32_t gx = 0; gx < 10; ++gx) {
        x_bounds[gx] = (uint32_t)(((uint64_t)gx * (uint64_t)w * 7282ULL) >> 16); /* 7282/65536 approx 1/9 */
    }

    /* Downsample to 9x8 grid using box area averaging */
    for (uint32_t gy = 0; gy < 8; ++gy) {
        uint32_t y0 = y_bounds[gy];
        uint32_t y1 = y_bounds[gy + 1];
        if (y1 <= y0) {
            y1 = y0 + 1U;
        }

        for (uint32_t gx = 0; gx < 9; ++gx) {
            uint32_t x0 = x_bounds[gx];
            uint32_t x1 = x_bounds[gx + 1];
            if (x1 <= x0) {
                x1 = x0 + 1U;
            }

            uint64_t block_sum = 0;
            uint32_t block_count = 0;

            for (uint32_t y = y0; y < y1; ++y) {
                const uint8_t *row = gray + (y * (uint32_t)w);
                for (uint32_t x = x0; x < x1; ++x) {
                    block_sum += row[x];
                    block_count++;
                }
            }

            sampled[gy][gx] = (uint8_t)(block_count > 0 ? (block_sum / block_count) : 0);
        }
    }

    /* Compute 64-bit horizontal gradient hash */
    uint64_t hash = 0;
    for (uint32_t r = 0; r < 8; ++r) {
        for (uint32_t c = 0; c < 8; ++c) {
            uint32_t bit_index = r * 8U + c;
            if (sampled[r][c] > sampled[r][c + 1]) {
                hash |= ((uint64_t)1ULL << bit_index);
            }
        }
    }

    return hash;
}

uint32_t safir_dhash_distance(uint64_t a, uint64_t b) {
    uint64_t diff = a ^ b;
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    return (uint32_t)__popcnt64(diff);
#elif defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_popcountll(diff);
#else
    diff = diff - ((diff >> 1) & 0x5555555555555555ULL);
    diff = (diff & 0x3333333333333333ULL) + ((diff >> 2) & 0x3333333333333333ULL);
    diff = (diff + (diff >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (uint32_t)((diff * 0x0101010101010101ULL) >> 56);
#endif
}

bool safir_dhash_is_similar(uint64_t a, uint64_t b) {
    return safir_dhash_distance(a, b) <= SAFIR_DHASH_SIMILAR_THRESHOLD;
}
