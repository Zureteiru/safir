#ifndef SAFIR_CROP_H
#define SAFIR_CROP_H

#include "safir_types.h"
#include "safir_ledger.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAFIR_DHASH_SIMILAR_THRESHOLD 3

/**
 * Extracts ONLY the `region` rectangle from a 1280x720 RGBA8888 buffer
 * and converts it to 8-bit luminance using the integer formula:
 *   Y = (77*R + 150*G + 29*B) >> 8
 * 
 * Automatically applies polarity inversion if the crop borders are dark,
 * ensuring glyphs are dark-on-light for OCR recognition.
 *
 * @param rgba_1280x720 Pointer to full screen RGBA8888 buffer (1280 * 720 * 4 bytes).
 * @param region Sub-rectangle to crop.
 * @param out_gray Output grayscale buffer (must hold at least region.w * region.h bytes).
 * @return true on success, false on invalid parameters or out-of-bounds coordinates.
 */
bool safir_crop_rgba_to_gray(const uint8_t *rgba_1280x720, SafirRect region, uint8_t *out_gray);

/**
 * Convenience helper that allocates region.w * region.h bytes from POOL_OCR_SCRATCH
 * and performs safir_crop_rgba_to_gray into it.
 *
 * @return Pointer to allocated grayscale buffer in POOL_OCR_SCRATCH, or NULL on failure.
 */
uint8_t *safir_crop_rgba_to_scratch(const uint8_t *rgba_1280x720, SafirRect region);

/**
 * Polarity auto-inversion (like Rubi_for_vita).
 * Evaluates the perimeter border pixels of the grayscale crop.
 * If the average border luminance is < 128 (light text on dark dialogue box),
 * inverts all pixels in-place (pixel = 255 - pixel) so glyphs are dark-on-light.
 *
 * @return true if inverted, false if left unchanged.
 */
bool safir_crop_auto_invert(uint8_t *gray, uint16_t w, uint16_t h);

/**
 * Downsamples the grayscale crop to 9x8 and computes a 64-bit horizontal gradient hash (dHash).
 * 
 * @param gray Pointer to grayscale image pixels.
 * @param w Width of grayscale image.
 * @param h Height of grayscale image.
 * @return 64-bit perceptual hash value.
 */
uint64_t safir_compute_dhash(const uint8_t *gray, uint16_t w, uint16_t h);

/**
 * Computes the Hamming distance (number of bit differences) between two 64-bit perceptual hashes
 * using hardware-accelerated popcount where available.
 */
uint32_t safir_dhash_distance(uint64_t a, uint64_t b);

/**
 * Evaluates whether two dHash values are perceptual matches (Hamming distance <= 3).
 * If true, OCR can be skipped and cached tokens reused in 0ms.
 */
bool safir_dhash_is_similar(uint64_t a, uint64_t b);

/* safir_jpeg_crop_gray() result codes (0 = success). */
#define SAFIR_JPEG_E_ARG          (-1)
#define SAFIR_JPEG_E_GEOMETRY     (-2)   /* region outside the decoded image */
#define SAFIR_JPEG_E_DECODE_BASE  (-100) /* -100 - picojpeg error code */

/**
 * Decodes only `rect` of a baseline JPEG screenshot into a 2x-downsampled greyscale
 * crop (((rect.w+1)/2) x ((rect.h+1)/2) bytes, 2x2 box averaged): the same layout
 * safir_capture_crop_grayscale() produces. Uses picojpeg (~2.4 KB static state, NOT
 * thread safe: hold the engine lock). Returns 0 or a SAFIR_JPEG_E_* code.
 */
int safir_jpeg_crop_gray(const uint8_t *jpeg, size_t jpeg_size, SafirRect rect, uint8_t *out_gray,
                         uint16_t *out_w, uint16_t *out_h);

/* One rectangle of a multi-rectangle decode: scale 1 = every pixel (w x h bytes),
 * scale 2 = 2x2 box average (((w+1)/2) x ((h+1)/2) bytes). */
typedef struct SafirJpegTarget {
    SafirRect rect;
    uint8_t scale;
    uint8_t *out;
} SafirJpegTarget;

/**
 * Decodes several rectangles of one JPEG in a single pass (the decoder is sequential, so
 * one pass is as fast as decoding the lowest rectangle once). Same rules as
 * safir_jpeg_crop_gray(); outputs must not overlap. Returns 0 or a SAFIR_JPEG_E_* code.
 */
int safir_jpeg_crop_multi(const uint8_t *jpeg, size_t jpeg_size, const SafirJpegTarget *targets, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* SAFIR_CROP_H */
