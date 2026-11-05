/*
 * core/crop/safir_jpeg_crop.c
 *
 * Decodes only a screen region of a baseline JPEG screenshot into the overlay's
 * 2x-downsampled greyscale crop (same layout as safir_capture_crop_grayscale()).
 *
 * Why: caps:sc's raw-screenshot stream requires debug mode (fails with 2206-0820 on
 * retail units), but capsscCaptureJpegScreenShot works on 10.0.0+. A full 1280x720
 * RGB decode would need 2.7 MB, so picojpeg (libs/picojpeg) decodes one MCU at a time
 * and each pixel inside the region is accumulated straight into the output.
 */
#include "safir_crop.h"
#include "picojpeg.h"
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} JpegSource;

static unsigned char jpeg_need_bytes(unsigned char *buf, unsigned char buf_size,
                                     unsigned char *actually_read, void *user) {
    JpegSource *src = (JpegSource *)user;
    size_t n = src->size - src->pos;
    if (n > buf_size) n = buf_size;
    memcpy(buf, src->data + src->pos, n);
    src->pos += n;
    *actually_read = (unsigned char)n;
    return 0;
}

static size_t target_w(const SafirJpegTarget *t) { return (size_t)((t->rect.w + t->scale - 1) / t->scale); }
static size_t target_h(const SafirJpegTarget *t) { return (size_t)((t->rect.h + t->scale - 1) / t->scale); }

int safir_jpeg_crop_multi(const uint8_t *jpeg, size_t jpeg_size, const SafirJpegTarget *targets, size_t count) {
    if (!jpeg || jpeg_size < 4 || !targets || count == 0) return SAFIR_JPEG_E_ARG;
    for (size_t i = 0; i < count; ++i) {
        const SafirJpegTarget *t = &targets[i];
        if (!t->out || t->rect.w == 0 || t->rect.h == 0 || (t->scale != 1 && t->scale != 2)) return SAFIR_JPEG_E_ARG;
    }

    JpegSource src = { jpeg, jpeg_size, 0 };
    pjpeg_image_info_t info;
    memset(&info, 0, sizeof(info));
    unsigned char rc = pjpeg_decode_init(&info, jpeg_need_bytes, &src, 0);
    if (rc != 0) return SAFIR_JPEG_E_DECODE_BASE - (int)rc;

    uint32_t y_end = 0;
    for (size_t i = 0; i < count; ++i) {
        const SafirJpegTarget *t = &targets[i];
        if ((uint32_t)t->rect.x + t->rect.w > (uint32_t)info.m_width ||
            (uint32_t)t->rect.y + t->rect.h > (uint32_t)info.m_height) {
            return SAFIR_JPEG_E_GEOMETRY;
        }
        if (t->scale == 2) memset(t->out, 0, target_w(t) * target_h(t));
        if ((uint32_t)t->rect.y + t->rect.h > y_end) y_end = (uint32_t)t->rect.y + t->rect.h;
    }

    for (int my = 0; my < info.m_MCUSPerCol; ++my) {
        const uint32_t mcu_y = (uint32_t)my * (uint32_t)info.m_MCUHeight;
        if (mcu_y >= y_end) break; /* everything below the lowest rectangle can be skipped */
        for (int mx = 0; mx < info.m_MCUSPerRow; ++mx) {
            rc = pjpeg_decode_mcu();
            if (rc == PJPG_NO_MORE_BLOCKS) goto done;
            if (rc != 0) return SAFIR_JPEG_E_DECODE_BASE - (int)rc;

            const uint32_t mcu_x = (uint32_t)mx * (uint32_t)info.m_MCUWidth;
            for (size_t i = 0; i < count; ++i) {
                const SafirJpegTarget *t = &targets[i];
                const uint32_t x0 = t->rect.x, x1 = (uint32_t)t->rect.x + t->rect.w;
                const uint32_t y0 = t->rect.y, y1 = (uint32_t)t->rect.y + t->rect.h;
                if (mcu_y + (uint32_t)info.m_MCUHeight <= y0 || mcu_y >= y1) continue;
                if (mcu_x >= x1 || mcu_x + (uint32_t)info.m_MCUWidth <= x0) continue;
                const size_t ow = target_w(t);

                for (int by = 0; by < info.m_MCUHeight; ++by) {
                    const uint32_t sy = mcu_y + (uint32_t)by;
                    if (sy < y0 || sy >= y1) continue;
                    for (int bx = 0; bx < info.m_MCUWidth; ++bx) {
                        const uint32_t sx = mcu_x + (uint32_t)bx;
                        if (sx < x0 || sx >= x1) continue;
                        /* picojpeg always lays an MCU out as a 2-wide grid of 8x8 blocks: the
                         * lower block row starts at +128 even when the MCU is one block wide
                         * (H1V2, which is what Switch screenshots use). */
                        const int off = (by >> 3) * 128 + (bx >> 3) * 64 + (by & 7) * 8 + (bx & 7);
                        uint32_t g;
                        if (info.m_scanType == PJPG_GRAYSCALE) {
                            g = info.m_pMCUBufR[off];
                        } else {
                            g = (77u * info.m_pMCUBufR[off] + 150u * info.m_pMCUBufG[off] +
                                 29u * info.m_pMCUBufB[off]) >> 8;
                        }
                        if (t->scale == 1) {
                            t->out[(size_t)(sy - y0) * ow + (sx - x0)] = (uint8_t)g;
                        } else {
                            /* 2x2 box average: each output pixel receives four quarter-weights. */
                            uint8_t *d = &t->out[(size_t)((sy - y0) >> 1) * ow + ((sx - x0) >> 1)];
                            uint32_t v = (uint32_t)*d + ((g + 2u) >> 2);
                            *d = (uint8_t)(v > 255u ? 255u : v);
                        }
                    }
                }
            }
        }
    }
done:
    /* 2x targets: odd widths/heights leave the last column/row with fewer samples. */
    for (size_t i = 0; i < count; ++i) {
        const SafirJpegTarget *t = &targets[i];
        if (t->scale != 2) continue;
        const size_t dw = target_w(t), dh = target_h(t);
        if (t->rect.w & 1) {
            for (size_t y = 0; y < dh; ++y) {
                uint32_t v = (uint32_t)t->out[y * dw + dw - 1] * 2u;
                t->out[y * dw + dw - 1] = (uint8_t)(v > 255u ? 255u : v);
            }
        }
        if (t->rect.h & 1) {
            for (size_t x = 0; x < dw; ++x) {
                uint32_t v = (uint32_t)t->out[(dh - 1) * dw + x] * 2u;
                t->out[(dh - 1) * dw + x] = (uint8_t)(v > 255u ? 255u : v);
            }
        }
    }
    return 0;
}

int safir_jpeg_crop_gray(const uint8_t *jpeg, size_t jpeg_size, SafirRect rect, uint8_t *out_gray,
                         uint16_t *out_w, uint16_t *out_h) {
    if (!jpeg || jpeg_size < 4 || !out_gray || rect.w == 0 || rect.h == 0) return SAFIR_JPEG_E_ARG;
    SafirJpegTarget t = { rect, 2, out_gray };
    int rc = safir_jpeg_crop_multi(jpeg, jpeg_size, &t, 1);
    if (rc != 0) return rc;
    if (out_w) *out_w = (uint16_t)target_w(&t);
    if (out_h) *out_h = (uint16_t)target_h(&t);
    return 0;
}
