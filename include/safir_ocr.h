#ifndef SAFIR_OCR_H
#define SAFIR_OCR_H

#include "safir_types.h"
#include "safir_ledger.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SafirDecoderMode {
    SAFIR_DECODER_GREEDY = 0,
    SAFIR_DECODER_BEAM   = 1
} SafirDecoderMode;

typedef struct SafirOcrConfig {
    SafirDecoderMode decoder_mode;
    uint8_t beam_width;    /* Beam width (e.g. 4) */
    float min_confidence;  /* Minimum confidence [0.0f .. 1.0f] */
    bool use_dhash_cache;  /* 0ms fast path when dHash distance <= 3 */
} SafirOcrConfig;

/* Forward declaration */
typedef struct SafirOcrBackend SafirOcrBackend;

/**
 * Vtable interface for pluggable OCR engines.
 */
struct SafirOcrBackend {
    const char *name;

    /**
     * Initializes backend weights and structures.
     * Backing memory is strictly allocated from POOL_OCR_WEIGHTS.
     */
    bool (*init)(SafirOcrBackend *self, const void *model_data, size_t model_bytes);

    /**
     * Spot horizontal text lines and recognize Japanese dialogue characters.
     * Maps all character bounding boxes from crop-space back to screen coordinates (1280x720).
     * 
     * @param self Backend instance.
     * @param gray_crop Grayscale crop buffer (from POOL_OCR_SCRATCH).
     * @param crop_w Width of crop in pixels.
     * @param crop_h Height of crop in pixels.
     * @param screen_region Screen rectangle where this crop was taken.
     * @param out_result Output struct receiving recognized text lines and char boxes.
     * @return true on success, false on error or empty.
     */
    bool (*recognize)(SafirOcrBackend *self,
                      const uint8_t *gray_crop,
                      uint16_t crop_w,
                      uint16_t crop_h,
                      SafirRect screen_region,
                      SafirOcrResult *out_result);

    /**
     * Resets backend state and clears the dHash cache.
     */
    void (*reset)(SafirOcrBackend *self);

    void *userdata;
};

/* ========================================================================= */
/* a) vocr_local: Pure-C INT8 Visual Novel OCR runtime                      */
/*    (textspot8 line spotter + VOCR6 INT8 CTC recognizer + decoder)        */
/* ========================================================================= */

#define VOCR_MAGIC 0x52434F56 /* "VOCR" */
#define VOCR_VERSION_6_2 0x00060002

typedef struct SafirVocrHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t model_type;
    uint32_t vocab_size;
    uint32_t feature_dim;
    uint32_t conv_layers;
    uint32_t conv_weight_offset;
    uint32_t conv_weight_size;
    uint32_t conv_bias_offset;
    uint32_t conv_bias_size;
    uint32_t fc_weight_offset;
    uint32_t fc_weight_size;
    uint32_t fc_bias_offset;
    uint32_t fc_bias_size;
    uint32_t scales_offset;
    uint32_t scales_size;
    uint32_t vocab_offset;
    uint32_t vocab_size_bytes;
    uint32_t glyph_template_offset;
    uint32_t glyph_template_size;
    uint32_t reserved[3];
} SafirVocrHeader;

/**
 * Creates and initializes the local VOCR engine in POOL_OCR_WEIGHTS.
 */
SafirOcrBackend *safir_ocr_vocr_create(const SafirOcrConfig *config);

/**
 * Loads and initializes VOCR6 model from an in-memory .vocr binary buffer.
 */
bool safir_ocr_vocr_load_binary(SafirOcrBackend *backend, const void *data, size_t size);

/**
 * Human-readable reason for the last safir_ocr_vocr_load_file() result (static buffer).
 */
const char *safir_ocr_vocr_last_error(void);

/**
 * True unless the most recently loaded backend runs the real VOCR6.2 network
 * (libs/vocr). The Switch UI labels placeholder output as [FAKE].
 */
bool safir_ocr_vocr_is_placeholder(void);

/**
 * Result of the embedded self-test run after a real model loads:
 * 1 passed, 0 failed (e.g. a miscompiled NEON path), -1 not run.
 */
int safir_ocr_vocr_selftest_result(void);

/**
 * Selects the line finder of the real engine (VOCR_LINES_* from libs/vocr/vocr_lines.h;
 * default VOCR_LINES_TEXTBOX, which also works on whole frames).
 */
void safir_ocr_vocr_set_line_finder(int finder);

/**
 * Loads VOCR6 model from a .vocr file on disk (or sdmc).
 */
bool safir_ocr_vocr_load_file(SafirOcrBackend *backend, const char *filepath);

/**
 * Lends the loaded model's memory (~800 KB in POOL_OCR_WEIGHTS) as scratch, e.g. for the
 * screenshot JPEG, so the overlay heap never has to hold it. Recognition refuses to run
 * until safir_ocr_vocr_restore_weights() has re-read the model from its file.
 * Returns NULL when no real model was loaded from a file.
 */
void *safir_ocr_vocr_lend_weights(SafirOcrBackend *backend, size_t *bytes);
bool safir_ocr_vocr_restore_weights(SafirOcrBackend *backend);

/* Two-pass scan of a JPEG screenshot (see safir_ocr_vocr.c). Pass 1 finds lines at half
 * resolution; pass 2 re-reads each line as its own band, at full resolution when small. */
#define SAFIR_SCAN_MAX_BLOCKS 12
typedef struct SafirScanBlock { /* one text line */
    SafirRect screen;   /* screen pixels covered */
    uint8_t scale;      /* 1 = full resolution, 2 = half */
    uint16_t w, h;      /* pixels in buf */
    uint16_t line_h;    /* line height, screen pixels */
    uint8_t dark;       /* 1 = dark text on a light background (real pixels) */
    uint32_t offset;    /* into buf */
} SafirScanBlock;
typedef struct SafirScanPlan {
    SafirScanBlock blocks[SAFIR_SCAN_MAX_BLOCKS];
    uint8_t count;
    uint8_t pass1_lines; /* lines found at half resolution */
} SafirScanPlan;

/**
 * Pass 1 + 2, no network weights needed (so it can run while the JPEG sits in lent model
 * memory). Finds lines in `region` at half resolution (decoded into buf, or already there
 * when half_ready), groups them into text blocks and decodes each block into buf again,
 * at full resolution for small text. Returns 0 (plan->count may be 0) or a negative error.
 */
int safir_scan_prepare(const uint8_t *jpeg, size_t jpeg_size, SafirRect region, uint8_t *buf, size_t buf_cap,
                       bool half_ready, SafirScanPlan *plan);

/* Host diagnostics: print pass-1 lines to stderr. */
void safir_scan_set_debug(bool on);

/**
 * Recognizes the prepared blocks. Lines that are not Japanese text (HUD numbers, Latin
 * labels, single glyphs from background art) are dropped.
 */
bool safir_ocr_vocr_recognize_plan(SafirOcrBackend *backend, const uint8_t *buf, const SafirScanPlan *plan,
                                   SafirOcrResult *out);

/**
 * Initializes a deterministic synthetic model for host testing and coordinate verification.
 * Generates known weights and mapping tables in POOL_OCR_WEIGHTS.
 */
bool safir_ocr_vocr_init_synthetic(SafirOcrBackend *backend,
                                   uint16_t vocab_size,
                                   const uint32_t *codepoints);

/**
 * Evaluates INT8 dot product / GEMM inner loop.
 * Uses ARM NEON (vmlal_s8 / vpadalq_s16) when compiling for __aarch64__,
 * and a portable scalar loop on host platforms.
 */
int32_t safir_dot_product_int8(const int8_t *a, const int8_t *b, int len, int32_t bias);

#ifdef __cplusplus
}
#endif

#endif /* SAFIR_OCR_H */
