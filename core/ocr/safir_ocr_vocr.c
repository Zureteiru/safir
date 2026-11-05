#include "safir_ocr.h"
#include "safir_crop.h"
#include "safir_ledger.h"
#include "safir_text.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include "vocr.h"
#include "vocr_lines.h"
#include "safir_ocr_selftest.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

/* ========================================================================= */
/* INT8 Dot Product / GEMM Inner Loop                                       */
/* ========================================================================= */

int32_t safir_dot_product_int8(const int8_t *a, const int8_t *b, int len, int32_t bias) {
#if defined(__aarch64__)
    int32x4_t acc32 = vdupq_n_s32(0);
    int i = 0;
    for (; i <= len - 16; i += 16) {
        int8x16_t va = vld1q_s8(a + i);
        int8x16_t vb = vld1q_s8(b + i);

        /* vmlal_s8 multiplies int8x8 vectors into int16x8 */
        int16x8_t acc16_low = vdupq_n_s16(0);
        acc16_low = vmlal_s8(acc16_low, vget_low_s8(va), vget_low_s8(vb));

        int16x8_t acc16_high = vdupq_n_s16(0);
        acc16_high = vmlal_s8(acc16_high, vget_high_s8(va), vget_high_s8(vb));

        /* vpadalq_s16 pairwise adds and accumulates int16x8 into int32x4 */
        acc32 = vpadalq_s16(acc32, acc16_low);
        acc32 = vpadalq_s16(acc32, acc16_high);
    }
    int32_t sum = vaddvq_s32(acc32) + bias;
    for (; i < len; ++i) {
        sum += (int32_t)a[i] * (int32_t)b[i];
    }
    return sum;
#else
    /* 4-way unrolled cache-friendly vector dot-product on Host */
    int32_t sum0 = bias;
    int32_t sum1 = 0;
    int32_t sum2 = 0;
    int32_t sum3 = 0;
    int i = 0;
    for (; i <= len - 4; i += 4) {
        sum0 += (int32_t)a[i + 0] * (int32_t)b[i + 0];
        sum1 += (int32_t)a[i + 1] * (int32_t)b[i + 1];
        sum2 += (int32_t)a[i + 2] * (int32_t)b[i + 2];
        sum3 += (int32_t)a[i + 3] * (int32_t)b[i + 3];
    }
    int32_t sum = (sum0 + sum1) + (sum2 + sum3);
    for (; i < len; ++i) {
        sum += (int32_t)a[i] * (int32_t)b[i];
    }
    return sum;
#endif
}

/* ========================================================================= */
/* Line Spotter: textspot8 horizontal line segmenter                         */
/* ========================================================================= */

#define MAX_SPOTTED_LINES 8

static uint16_t textspot8_spot_lines(const uint8_t *gray,
                                     uint16_t w,
                                     uint16_t h,
                                     SafirRect *out_lines,
                                     uint16_t max_lines) {
    if (!gray || w == 0 || h == 0 || !out_lines || max_lines == 0) {
        return 0;
    }

    /* Compute horizontal projection profile: count dark ink pixels (< 128) per row */
    uint16_t spotted = 0;
    bool in_line = false;
    uint16_t start_y = 0;
    uint16_t gap_count = 0;
    const uint16_t ink_threshold = (w > 200) ? (w / 40) : 5; /* At least ~2.5% ink */

    for (uint16_t y = 0; y < h; ++y) {
        const uint8_t *row = gray + (uint32_t)y * (uint32_t)w;
        uint16_t ink = 0;
        for (uint16_t x = 0; x < w; ++x) {
            if (row[x] < 128) {
                ink++;
            }
        }

        bool has_ink = (ink >= ink_threshold);

        if (has_ink) {
            if (!in_line) {
                in_line = true;
                start_y = y;
            }
            gap_count = 0;
        } else {
            if (in_line) {
                gap_count++;
                /* Bridge small intra-line gaps (accents, punctuation) */
                if (gap_count > 4 || y == h - 1) {
                    uint16_t line_h = (uint16_t)(y - gap_count - start_y + 1);
                    if (line_h >= 8 && spotted < max_lines) {
                        /* Find horizontal bounds of this text band */
                        uint16_t min_x = w;
                        uint16_t max_x = 0;
                        for (uint16_t ly = start_y; ly <= y - gap_count; ++ly) {
                            const uint8_t *lrow = gray + (uint32_t)ly * (uint32_t)w;
                            for (uint16_t lx = 0; lx < w; ++lx) {
                                if (lrow[lx] < 128) {
                                    if (lx < min_x) min_x = lx;
                                    if (lx > max_x) max_x = lx;
                                }
                            }
                        }

                        if (max_x > min_x) {
                            /* Add padding */
                            uint16_t pad_x = 2;
                            uint16_t lx = (min_x > pad_x) ? (uint16_t)(min_x - pad_x) : 0;
                            uint16_t lw = (uint16_t)(max_x - lx + pad_x + 1);
                            if ((uint32_t)lx + lw > w) {
                                lw = (uint16_t)(w - lx);
                            }

                            out_lines[spotted].x = lx;
                            out_lines[spotted].y = start_y;
                            out_lines[spotted].w = lw;
                            out_lines[spotted].h = line_h;
                            spotted++;
                        }
                    }
                    in_line = false;
                    gap_count = 0;
                }
            }
        }
    }

    /* Fallback: if no line found but crop is non-empty, use full crop */
    if (spotted == 0 && w >= 10 && h >= 10) {
        out_lines[0].x = 0;
        out_lines[0].y = 0;
        out_lines[0].w = w;
        out_lines[0].h = h;
        spotted = 1;
    }

    return spotted;
}

/* ========================================================================= */
/* VOCR6 INT8 Model and Engine Context                                      */
/* ========================================================================= */

/* The PLACEHOLDER engine (used only for synthetic test vocabularies) keeps its
 * tiny vocabulary here. The real engine reads the vocabulary from the blob. */
#define VOCR_MAX_VOCAB 64
#define VOCR_MAX_TIME_FRAMES 128
#define VOCR_FEATURE_DIM 16

typedef struct SafirVocrModel {
    uint16_t vocab_size;
    uint32_t codepoints[VOCR_MAX_VOCAB];
    int8_t conv_weights[VOCR_FEATURE_DIM * VOCR_FEATURE_DIM];
    int32_t conv_bias[VOCR_FEATURE_DIM];
    int8_t fc_weights[VOCR_FEATURE_DIM * VOCR_FEATURE_DIM];
    int32_t fc_bias[VOCR_FEATURE_DIM];
    const uint8_t *raw_blob;
    size_t raw_blob_size;
    bool initialized;
} SafirVocrModel;

typedef struct SafirDhashCache {
    uint64_t last_dhash;
    SafirOcrResult cached_result;
    bool valid;
} SafirDhashCache;

typedef struct SafirVocrContext {
    SafirOcrConfig config;
    SafirVocrModel model;   /* placeholder engine state */
    SafirDhashCache cache;
    bool real;              /* true: run the VOCR6.2 network from libs/vocr */
    int selftest;           /* -1 not run, 0 failed, 1 passed */
    vocr_model vm;          /* parsed blob; points into POOL_OCR_WEIGHTS */
    /* The blob's memory can be lent out (safir_ocr_vocr_lend_weights) as scratch for the
     * 512 KB screenshot and re-read from the file afterwards. */
    void *blob;
    size_t blob_bytes;
    char blob_path[128];
    bool lent;
} SafirVocrContext;

/* Arithmetic and tiling for the real engine: int8 weights with exact int32 sums,
 * one CTC frame per tile and per head batch. Smallest arena (53,856 B for
 * VOCR6.2_S_w8) at practically the same speed as the default tiling. */
static void real_engine_opts(vocr_opts *o) {
    memset(o, 0, sizeof(*o));
    o->mode = VOCR_MODE_W8I;
    o->tile_frames = 1;
    o->head_frames = 1;
}

/* Line finder used by the real engine. TEXTBOX keeps GRAD's rows and repairs textbox
 * topology; unlike GRAD it isolates dialogue lines on a full 1280x720 frame instead of
 * merging the background art into one block. */
static int s_line_finder = VOCR_LINES_TEXTBOX;

void safir_ocr_vocr_set_line_finder(int finder) {
    s_line_finder = finder;
}

/* Upper bound on CTC frames of any line (max_width / stride, plus padding). */
#define REAL_MAX_FRAMES 160

static uint32_t decode_utf8_char(const uint8_t *s, size_t len) {
    if (!s || len == 0) return 0;
    if ((s[0] & 0x80) == 0) return s[0];
    if ((s[0] & 0xE0) == 0xC0 && len >= 2) {
        return (((uint32_t)s[0] & 0x1F) << 6) | ((uint32_t)s[1] & 0x3F);
    }
    if ((s[0] & 0xF0) == 0xE0 && len >= 3) {
        return (((uint32_t)s[0] & 0x0F) << 12) | (((uint32_t)s[1] & 0x3F) << 6) | ((uint32_t)s[2] & 0x3F);
    }
    if ((s[0] & 0xF8) == 0xF0 && len >= 4) {
        return (((uint32_t)s[0] & 0x07) << 18) | (((uint32_t)s[1] & 0x3F) << 12) | (((uint32_t)s[2] & 0x3F) << 6) | ((uint32_t)s[3] & 0x3F);
    }
    return 0;
}

static char s_ocr_last_error[160] = "no load attempted";
#define OCR_ERR(...) snprintf(s_ocr_last_error, sizeof(s_ocr_last_error), __VA_ARGS__)

/* State of the most recently loaded backend, for the Switch UI. */
static bool s_last_load_real = false;
static int s_last_selftest = -1;

const char *safir_ocr_vocr_last_error(void) {
    return s_ocr_last_error;
}

bool safir_ocr_vocr_is_placeholder(void) {
    return !s_last_load_real;
}

int safir_ocr_vocr_selftest_result(void) {
    return s_last_selftest;
}

static bool real_recognize_line(SafirVocrContext *ctx, vocr_ctx *vc, const uint8_t *pixels,
                                unsigned w, unsigned h, size_t stride, int dark,
                                char *text, size_t text_cap, uint16_t *best, unsigned *frames,
                                unsigned *resized_w, float *conf);

/* Runs the embedded self-test line through the network. Catches a miscompiled
 * NEON path on the console before the user trusts the output. */
static int run_selftest(SafirVocrContext *ctx) {
    vocr_opts o;
    real_engine_opts(&o);
    size_t need = vocr_arena_bytes(&ctx->vm, &o);
    size_t mark = safir_pool_used(POOL_OCR_SCRATCH);
    void *arena = safir_alloc(POOL_OCR_SCRATCH, need);
    if (!arena) {
        OCR_ERR("self-test: POOL_OCR_SCRATCH OOM (need %zu B)", need);
        return 0;
    }
    vocr_ctx vc;
    int rc = vocr_init(&vc, &ctx->vm, &o, arena, need);
    int pass = 0;
    if (rc == VOCR_OK) {
        char text[128];
        uint16_t best[REAL_MAX_FRAMES];
        unsigned frames = 0, rw = 0;
        float conf = 0.0f;
        if (real_recognize_line(ctx, &vc, s_safir_ocr_selftest_pixels, SAFIR_OCR_SELFTEST_W,
                                SAFIR_OCR_SELFTEST_H_PX, SAFIR_OCR_SELFTEST_W, 0,
                                text, sizeof(text), best, &frames, &rw, &conf)) {
            pass = (strcmp(text, s_safir_ocr_selftest_text) == 0);
        }
    }
    safir_pool_rewind(POOL_OCR_SCRATCH, mark);
    return pass;
}

/* Parses a real VOCR blob that already sits 16-byte aligned in POOL_OCR_WEIGHTS. */
static bool real_engine_load(SafirVocrContext *ctx, const void *blob, size_t bytes) {
    int rc = vocr_load(&ctx->vm, blob, bytes);
    if (rc != VOCR_OK) {
        OCR_ERR("vocr_load: %s (%d)", vocr_strerror(rc), rc);
        return false;
    }
    vocr_opts o;
    real_engine_opts(&o);
    size_t need = vocr_arena_bytes(&ctx->vm, &o);
    if (need == 0) {
        OCR_ERR("blob does not support int8 W8I mode");
        return false;
    }
    ctx->real = true;
    ctx->model.initialized = true;
    ctx->cache.valid = false;
    ctx->selftest = run_selftest(ctx);
    s_last_load_real = true;
    s_last_selftest = ctx->selftest;
    OCR_ERR("ok: VOCR %u classes, h=%u, arena %zu B, self-test %s",
            ctx->vm.n_classes, ctx->vm.height, need, ctx->selftest ? "PASS" : "FAIL");
    return true;
}

bool safir_ocr_vocr_load_binary(SafirOcrBackend *backend, const void *data, size_t size) {
    if (!backend || !backend->userdata || !data || size < 80) {
        return false;
    }
    SafirVocrContext *ctx = (SafirVocrContext *)backend->userdata;
    SafirVocrModel *m = &ctx->model;

    const uint32_t *u32 = (const uint32_t *)data;
    if (u32[0] != VOCR_MAGIC) {
        return false;
    }

    /* A real exported blob: run the actual network (requires 16-byte alignment). */
    {
        vocr_model probe;
        if (((uintptr_t)data & 15u) == 0 && vocr_load(&probe, data, size) == VOCR_OK) {
            return real_engine_load(ctx, data, size);
        }
    }
    ctx->real = false;
    s_last_load_real = false;
    s_last_selftest = -1;

    uint32_t ver = u32[1];
    m->raw_blob = (const uint8_t *)data;
    m->raw_blob_size = size;

    if (ver == 0x400001) {
        /* Real VOCR6.2 release blob format */
        uint32_t vocab_size = u32[6];
        uint32_t vocab_off = u32[8];
        uint32_t vocab_len = u32[9];

        if (vocab_size > VOCR_MAX_VOCAB) vocab_size = VOCR_MAX_VOCAB;
        m->vocab_size = (uint16_t)vocab_size;

        if (vocab_off + vocab_size * 2 <= size) {
            const uint16_t *offsets = (const uint16_t *)((const uint8_t *)data + vocab_off);
            const uint8_t *str_table = (const uint8_t *)data + vocab_off + vocab_size * 2;
            size_t str_cap = size - (vocab_off + vocab_size * 2);

            m->codepoints[0] = 0; /* Blank token */

            for (uint32_t i = 1; i < vocab_size; ++i) {
                uint16_t start = offsets[i];
                uint16_t end = (i + 1 < vocab_size) ? offsets[i + 1] : (uint16_t)vocab_len;
                if (start >= str_cap || end > str_cap || start >= end) {
                    m->codepoints[i] = 0x3000;
                    continue;
                }

                size_t c_len = end - start;
                const uint8_t *raw = str_table + start;

                /* Handle byte ordering in real VOCR6.2 vocabulary */
                if (c_len == 3 && raw[2] >= 0xE0) {
                    uint8_t rot[3] = { raw[2], raw[0], raw[1] };
                    m->codepoints[i] = decode_utf8_char(rot, 3);
                } else if (c_len == 2 && raw[1] >= 0xC0) {
                    uint8_t rot[2] = { raw[1], raw[0] };
                    m->codepoints[i] = decode_utf8_char(rot, 2);
                } else if (c_len >= 2 && raw[c_len - 1] >= 0xC0) {
                    uint8_t rev[8];
                    for (size_t b = 0; b < c_len && b < sizeof(rev); ++b) {
                        rev[b] = raw[c_len - 1 - b];
                    }
                    m->codepoints[i] = decode_utf8_char(rev, c_len);
                } else {
                    m->codepoints[i] = decode_utf8_char(raw, c_len);
                }
            }
        }
    } else {
        /* Synthetic VOCR6.2 model format (from fetch_or_synth_models.py) */
        const SafirVocrHeader *hdr = (const SafirVocrHeader *)data;
        uint32_t vocab_size = hdr->vocab_size;
        if (vocab_size > VOCR_MAX_VOCAB) vocab_size = VOCR_MAX_VOCAB;
        m->vocab_size = (uint16_t)vocab_size;

        if (hdr->vocab_offset + hdr->vocab_size_bytes <= size) {
            const uint32_t *cps = (const uint32_t *)((const uint8_t *)data + hdr->vocab_offset);
            for (uint32_t i = 0; i < vocab_size; ++i) {
                m->codepoints[i] = cps[i];
            }
        }
    }

    /* Initialize convolution and FC projection weights */
    for (int i = 0; i < VOCR_FEATURE_DIM * VOCR_FEATURE_DIM; ++i) {
        m->conv_weights[i] = (int8_t)((i % 7) - 3);
    }
    for (int i = 0; i < VOCR_FEATURE_DIM; ++i) {
        m->conv_bias[i] = (int32_t)(i + 1);
    }

    m->initialized = true;
    return true;
}

bool safir_ocr_vocr_load_file(SafirOcrBackend *backend, const char *filepath) {
    if (!backend || !backend->userdata || !filepath) { OCR_ERR("bad argument"); return false; }

    errno = 0;
    FILE *f = fopen(filepath, "rb");
    if (!f) { int e = errno; OCR_ERR("fopen failed errno=%d (%s)", e, strerror(e)); return false; }

    long fsize = -1;
    if (fseek(f, 0, SEEK_END) == 0) fsize = ftell(f);
    if (fsize <= 80 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        OCR_ERR("file too small or unseekable (%ld B)", fsize);
        return false;
    }

    /* The network reads its weights in place, so the whole blob lives in
     * POOL_OCR_WEIGHTS (safir_alloc returns 16-byte aligned memory, as vocr_load needs). */
    size_t free_bytes = safir_pool_capacity(POOL_OCR_WEIGHTS) - safir_pool_used(POOL_OCR_WEIGHTS);
    void *buf = safir_alloc(POOL_OCR_WEIGHTS, (size_t)fsize);
    if (!buf) {
        fclose(f);
        OCR_ERR("POOL_OCR_WEIGHTS OOM: model %ld B, free %zu of %zu B", fsize, free_bytes,
                safir_pool_capacity(POOL_OCR_WEIGHTS));
        return false;
    }
    size_t rd = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (rd != (size_t)fsize) {
        OCR_ERR("short read %zu of %ld B", rd, fsize);
        return false;
    }

    if (((const uint32_t *)buf)[0] != VOCR_MAGIC) {
        OCR_ERR("magic mismatch 0x%08X (want VOCR)", (unsigned)((const uint32_t *)buf)[0]);
        return false;
    }

    SafirVocrContext *ctx = (SafirVocrContext *)backend->userdata;
    vocr_model probe;
    int rc = vocr_load(&probe, buf, (size_t)fsize);
    if (rc == VOCR_OK) {
        ctx->blob = buf;
        ctx->blob_bytes = (size_t)fsize;
        ctx->lent = false;
        snprintf(ctx->blob_path, sizeof(ctx->blob_path), "%s", filepath);
        return real_engine_load(ctx, buf, (size_t)fsize);
    }
    /* Not a real export: accept the synthetic test format for the placeholder engine. */
    if (((const uint32_t *)buf)[1] == VOCR_VERSION_6_2 &&
        safir_ocr_vocr_load_binary(backend, buf, (size_t)fsize)) {
        OCR_ERR("ok: synthetic test model (PLACEHOLDER engine)");
        return true;
    }
    OCR_ERR("vocr_load: %s (%d)", vocr_strerror(rc), rc);
    return false;
}

void *safir_ocr_vocr_lend_weights(SafirOcrBackend *backend, size_t *bytes) {
    if (bytes) *bytes = 0;
    if (!backend || !backend->userdata) return NULL;
    SafirVocrContext *ctx = (SafirVocrContext *)backend->userdata;
    if (!ctx->real || !ctx->blob || !ctx->blob_path[0]) return NULL;
    ctx->lent = true;
    if (bytes) *bytes = ctx->blob_bytes;
    return ctx->blob;
}

bool safir_ocr_vocr_restore_weights(SafirOcrBackend *backend) {
    if (!backend || !backend->userdata) return false;
    SafirVocrContext *ctx = (SafirVocrContext *)backend->userdata;
    if (!ctx->lent) return true;
    errno = 0;
    FILE *f = fopen(ctx->blob_path, "rb");
    if (!f) {
        int e = errno;
        OCR_ERR("re-reading model: fopen failed errno=%d (%s)", e, strerror(e));
        return false;
    }
    size_t rd = fread(ctx->blob, 1, ctx->blob_bytes, f);
    fclose(f);
    if (rd != ctx->blob_bytes) {
        OCR_ERR("re-reading model: short read %zu of %zu B", rd, ctx->blob_bytes);
        return false;
    }
    int rc = vocr_load(&ctx->vm, ctx->blob, ctx->blob_bytes);
    if (rc != VOCR_OK) {
        OCR_ERR("re-reading model: vocr_load: %s (%d)", vocr_strerror(rc), rc);
        return false;
    }
    ctx->lent = false;
    return true;
}

static bool backend_vocr_init(SafirOcrBackend *self, const void *model_data, size_t model_bytes) {
    if (!self || !self->userdata) {
        return false;
    }

    if (model_data && model_bytes >= 80) {
        const uint32_t *u32 = (const uint32_t *)model_data;
        if (u32[0] == VOCR_MAGIC) {
            return safir_ocr_vocr_load_binary(self, model_data, model_bytes);
        }
    }

    /* Default synthetic model if no external model passed */
    return safir_ocr_vocr_init_synthetic(self, 0, NULL);
}

bool safir_ocr_vocr_init_synthetic(SafirOcrBackend *backend,
                                   uint16_t vocab_size,
                                   const uint32_t *codepoints) {
    if (!backend || !backend->userdata) {
        return false;
    }
    SafirVocrContext *ctx = (SafirVocrContext *)backend->userdata;
    SafirVocrModel *m = &ctx->model;
    ctx->real = false;
    s_last_load_real = false;
    s_last_selftest = -1;

    /* Japanese visual novel test vocabulary */
    static const uint32_t s_default_japanese_vocab[] = {
        0,       /* CTC Blank */
        0x300C,  /* 「 */
        0x9B54,  /* 魔 */
        0x6CD5,  /* 法 */
        0x4F7F,  /* 使 */
        0x3044,  /* い */
        0x306E,  /* の */
        0x591C,  /* 夜 */
        0x3001,  /* 、 */
        0x661F,  /* 星 */
        0x304C,  /* が */
        0x7DB6,  /* 綺 */
        0x9E97,  /* 麗 */
        0x3067,  /* で */
        0x3059,  /* す */
        0x306D,  /* ね */
        0x300D,  /* 」 */
        0x65E5,  /* 日 */
        0x672C,  /* 本 */
        0x8A9E,  /* 語 */
        0x5922,  /* 夢 */
        0x3092,  /* を */
        0x898B,  /* 見 */
        0x308B,  /* る */
        0x5927,  /* 大 */
        0x597D,  /* 好 */
        0x98DF,  /* 食 */
        0x3079,  /* べ */
        0x884C,  /* 行 */
        0x7F8E   /* 美 */
    };

    uint16_t v_count = vocab_size;
    const uint32_t *v_points = codepoints;

    if (v_count == 0 || !v_points) {
        v_count = (uint16_t)(sizeof(s_default_japanese_vocab) / sizeof(s_default_japanese_vocab[0]));
        v_points = s_default_japanese_vocab;
    }

    if (v_count > VOCR_MAX_VOCAB) {
        v_count = VOCR_MAX_VOCAB;
    }

    m->vocab_size = v_count;
    for (uint16_t i = 0; i < v_count; ++i) {
        m->codepoints[i] = v_points[i];
    }

    /* Initialize deterministic synthetic weights */
    for (int i = 0; i < VOCR_FEATURE_DIM * VOCR_FEATURE_DIM; ++i) {
        m->conv_weights[i] = (int8_t)((i % 7) - 3);
    }
    for (int i = 0; i < VOCR_FEATURE_DIM; ++i) {
        m->conv_bias[i] = (int32_t)(i + 1);
    }

    m->initialized = true;
    return true;
}

static void vocr_reset(SafirOcrBackend *self) {
    if (!self || !self->userdata) {
        return;
    }
    SafirVocrContext *ctx = (SafirVocrContext *)self->userdata;
    ctx->cache.valid = false;
    ctx->cache.last_dhash = 0;
    memset(&ctx->cache.cached_result, 0, sizeof(SafirOcrResult));
}

static bool placeholder_recognize(SafirOcrBackend *self,
                           const uint8_t *gray_crop,
                           uint16_t crop_w,
                           uint16_t crop_h,
                           SafirRect screen_region,
                           SafirOcrResult *out_result) {
    if (!self || !self->userdata || !gray_crop || !out_result) {
        return false;
    }
    if (crop_w == 0 || crop_h == 0) {
        return false;
    }

    SafirVocrContext *ctx = (SafirVocrContext *)self->userdata;

    /* Step 1: Perceptual Hash (dHash) & 0ms Cache Check */
    uint64_t current_dhash = safir_compute_dhash(gray_crop, crop_w, crop_h);
    if (ctx->config.use_dhash_cache && ctx->cache.valid) {
        uint32_t dist = safir_dhash_distance(current_dhash, ctx->cache.last_dhash);
        if (dist <= SAFIR_DHASH_SIMILAR_THRESHOLD) {
            /* 0ms fast-path: reuse cached tokens */
            memcpy(out_result, &ctx->cache.cached_result, sizeof(SafirOcrResult));
            out_result->dhash = current_dhash;
            out_result->cached = true;
            return true;
        }
    }

    memset(out_result, 0, sizeof(SafirOcrResult));
    out_result->dhash = current_dhash;
    out_result->cached = false;

    if (!ctx->model.initialized) {
        safir_ocr_vocr_init_synthetic(self, 0, NULL);
    }

    /* Step 2: Line Spotting (textspot8) */
    SafirRect spotted_lines[MAX_SPOTTED_LINES];
    uint16_t line_count = textspot8_spot_lines(gray_crop, crop_w, crop_h, spotted_lines, MAX_SPOTTED_LINES);
    if (line_count == 0) {
        return false;
    }

    /* Step 3: CTC Recognition & Character Bounding Box Mapping */
    out_result->line_count = 0;

    for (uint16_t l = 0; l < line_count && out_result->line_count < SAFIR_MAX_LINES_PER_BOX; ++l) {
        SafirRect line_box = spotted_lines[l];
        SafirTextLine *text_line = &out_result->lines[out_result->line_count];
        text_line->char_count = 0;
        text_line->dhash = current_dhash;

        /* Estimate character width based on line height */
        uint16_t char_size = line_box.h;
        if (char_size < 12) char_size = 12;
        uint16_t num_chars_est = (uint16_t)(line_box.w / char_size);
        if (num_chars_est == 0) num_chars_est = 1;
        if (num_chars_est > SAFIR_MAX_CHARS_PER_LINE) num_chars_est = SAFIR_MAX_CHARS_PER_LINE;

        uint16_t time_frames = (uint16_t)(num_chars_est * 4);
        if (time_frames > VOCR_MAX_TIME_FRAMES) time_frames = VOCR_MAX_TIME_FRAMES;
        if (time_frames < 4) time_frames = 4;

        uint16_t prev_token = 0;
        uint16_t run_start = 0;

        for (uint16_t t = 0; t < time_frames; ++t) {
            /* Sample 16-element normalized feature slice from the crop */
            int8_t feature_slice[VOCR_FEATURE_DIM];
            uint16_t slice_x = line_box.x + (uint16_t)((uint32_t)t * line_box.w / time_frames);
            if (slice_x >= crop_w) slice_x = (uint16_t)(crop_w - 1);

            for (int k = 0; k < VOCR_FEATURE_DIM; ++k) {
                uint16_t sample_y = line_box.y + (uint16_t)((uint32_t)k * line_box.h / VOCR_FEATURE_DIM);
                if (sample_y >= crop_h) sample_y = (uint16_t)(crop_h - 1);
                uint8_t pix = gray_crop[(uint32_t)sample_y * crop_w + slice_x];
                feature_slice[k] = (int8_t)((int)pix - 128);
            }

            /* PLACEHOLDER ENGINE, NOT REAL INFERENCE: used only after init_synthetic() or a
             * synthetic test blob. The emitted token is chosen from the horizontal character
             * slot; real blobs go through real_recognize() and libs/vocr instead. */
            int32_t conv_act[VOCR_FEATURE_DIM];
            for (int oc = 0; oc < VOCR_FEATURE_DIM; ++oc) {
                const int8_t *w_row = &ctx->model.conv_weights[oc * VOCR_FEATURE_DIM];
                conv_act[oc] = safir_dot_product_int8(feature_slice, w_row, VOCR_FEATURE_DIM, ctx->model.conv_bias[oc]);
            }
            (void)conv_act;

            /* Check if there is significant ink in this time frame */
            bool frame_has_ink = false;
            for (int k = 0; k < VOCR_FEATURE_DIM; ++k) {
                if (feature_slice[k] < -20) { /* Dark glyph ink pixel */
                    frame_has_ink = true;
                    break;
                }
            }

            uint16_t top_token = 0;
            int32_t max_logit = -999999;

            if (frame_has_ink) {
                /* Map horizontal character position to vocabulary token */
                uint16_t char_slot = (uint16_t)((slice_x - line_box.x) / char_size);
                uint16_t token_idx = (uint16_t)(1 + (char_slot % (ctx->model.vocab_size - 1)));
                top_token = token_idx;
                max_logit = 100;
            } else {
                top_token = 0; /* CTC blank */
                max_logit = 50;
            }

            /* CTC Greedy Decoder / Beam Decoder Collapse */
            if (top_token != prev_token) {
                if (prev_token != 0 && text_line->char_count < SAFIR_MAX_CHARS_PER_LINE) {
                    uint16_t c_crop_x = line_box.x + (uint16_t)((uint32_t)run_start * line_box.w / time_frames);
                    uint16_t c_crop_w = (uint16_t)((uint32_t)(t - run_start) * line_box.w / time_frames);
                    if (c_crop_w == 0) c_crop_w = 1;
                    uint16_t c_crop_y = line_box.y;
                    uint16_t c_crop_h = line_box.h;

                    SafirCharBox *cb = &text_line->chars[text_line->char_count];
                    cb->codepoint = ctx->model.codepoints[prev_token];
                    cb->confidence = (max_logit >= 100) ? 0.95f : 0.75f;

                    /* Exact coordinate mapping from crop-space back to screen-space (handles 2x downsampling) */
                    uint32_t scale_x_num = (crop_w > 0) ? (uint32_t)screen_region.w : 1U;
                    uint32_t scale_x_den = (crop_w > 0) ? (uint32_t)crop_w : 1U;
                    uint32_t scale_y_num = (crop_h > 0) ? (uint32_t)screen_region.h : 1U;
                    uint32_t scale_y_den = (crop_h > 0) ? (uint32_t)crop_h : 1U;

                    cb->screen_box.x = (uint16_t)(screen_region.x + ((uint32_t)c_crop_x * scale_x_num / scale_x_den));
                    cb->screen_box.y = (uint16_t)(screen_region.y + ((uint32_t)c_crop_y * scale_y_num / scale_y_den));
                    cb->screen_box.w = (uint16_t)((uint32_t)c_crop_w * scale_x_num / scale_x_den);
                    cb->screen_box.h = (uint16_t)((uint32_t)c_crop_h * scale_y_num / scale_y_den);

                    text_line->char_count++;
                }
                prev_token = top_token;
                run_start = t;
            }
        }

        /* Flush final token if not blank */
        if (prev_token != 0 && text_line->char_count < SAFIR_MAX_CHARS_PER_LINE) {
            uint16_t c_crop_x = line_box.x + (uint16_t)((uint32_t)run_start * line_box.w / time_frames);
            uint16_t c_crop_w = (uint16_t)((uint32_t)(time_frames - run_start) * line_box.w / time_frames);
            if (c_crop_w == 0) c_crop_w = 1;
            uint16_t c_crop_y = line_box.y;
            uint16_t c_crop_h = line_box.h;

            SafirCharBox *cb = &text_line->chars[text_line->char_count];
            cb->codepoint = ctx->model.codepoints[prev_token];
            cb->confidence = 0.90f;

            uint32_t scale_x_num = (crop_w > 0) ? (uint32_t)screen_region.w : 1U;
            uint32_t scale_x_den = (crop_w > 0) ? (uint32_t)crop_w : 1U;
            uint32_t scale_y_num = (crop_h > 0) ? (uint32_t)screen_region.h : 1U;
            uint32_t scale_y_den = (crop_h > 0) ? (uint32_t)crop_h : 1U;

            cb->screen_box.x = (uint16_t)(screen_region.x + ((uint32_t)c_crop_x * scale_x_num / scale_x_den));
            cb->screen_box.y = (uint16_t)(screen_region.y + ((uint32_t)c_crop_y * scale_y_num / scale_y_den));
            cb->screen_box.w = (uint16_t)((uint32_t)c_crop_w * scale_x_num / scale_x_den);
            cb->screen_box.h = (uint16_t)((uint32_t)c_crop_h * scale_y_num / scale_y_den);

            text_line->char_count++;
        }

        if (text_line->char_count > 0) {
            out_result->line_count++;
        }
    }

    /* Save into dHash cache */
    if (out_result->line_count > 0) {
        memcpy(&ctx->cache.cached_result, out_result, sizeof(SafirOcrResult));
        ctx->cache.last_dhash = current_dhash;
        ctx->cache.valid = true;
    }

    return (out_result->line_count > 0);
}

/* ========================================================================= */
/* Real VOCR6.2 engine (libs/vocr): line finder + CTC network + char boxes    */
/* ========================================================================= */

static bool real_recognize_line(SafirVocrContext *ctx, vocr_ctx *vc, const uint8_t *pixels,
                                unsigned w, unsigned h, size_t stride, int dark,
                                char *text, size_t text_cap, uint16_t *best, unsigned *frames,
                                unsigned *resized_w, float *conf) {
    /* Top-4 per frame so conf_mean is a real softmax confidence (with top-1 it is always 1). */
    float topk_logit[REAL_MAX_FRAMES * 4];
    uint16_t topk_class[REAL_MAX_FRAMES * 4];
    vocr_extra ex;
    memset(&ex, 0, sizeof(ex));
    ex.best = best;
    ex.cap_frames = REAL_MAX_FRAMES;
    ex.topk = 4;
    ex.topk_logit = topk_logit;
    ex.topk_class = topk_class;

    vocr_image im;
    im.pixels = pixels;
    im.width = w;
    im.height = h;
    im.stride = stride;
    im.format = VOCR_GREY8 | (dark ? VOCR_INVERT : 0);

    (void)ctx;
    int rc = vocr_run(vc, &im, text, text_cap, &ex);
    if (rc != VOCR_OK && rc != VOCR_E_TEXT) {
        return false;
    }
    *frames = ex.frames;
    *resized_w = ex.width;
    *conf = ex.conf_mean;
    return true;
}

static size_t utf8_decode_one(const char *s, size_t len, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s;
    if (len == 0) return 0;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && len >= 2) { *cp = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F); return 2; }
    if ((u[0] & 0xF0) == 0xE0 && len >= 3) {
        *cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && len >= 4) {
        *cp = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) |
              ((uint32_t)(u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

static void map_crop_box_to_screen(SafirCharBox *cb, uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch,
                                   uint16_t crop_w, uint16_t crop_h, SafirRect region) {
    cb->screen_box.x = (uint16_t)(region.x + cx * region.w / crop_w);
    cb->screen_box.y = (uint16_t)(region.y + cy * region.h / crop_h);
    cb->screen_box.w = (uint16_t)(cw * region.w / crop_w);
    cb->screen_box.h = (uint16_t)(ch * region.h / crop_h);
    if (cb->screen_box.w == 0) cb->screen_box.w = 1;
    if (cb->screen_box.h == 0) cb->screen_box.h = 1;
}

/* Display width of a character in its line's cells: CJK, kana and full-width forms take a
 * whole cell, ASCII and half-width forms half of one. */
static unsigned cell_halves(uint32_t cp) {
    if (cp < 0x1100) return 1;                       /* Latin, digits, ASCII punctuation */
    if (cp >= 0xFF61 && cp <= 0xFFDC) return 1;      /* half-width katakana */
    if (cp >= 0xFFE8 && cp <= 0xFFEE) return 1;
    return 2;
}

/*
 * VN text is set in fixed-width cells. The CTC path gives each character's position, but
 * the network emits a character a little after its glyph begins, so those boxes sat about
 * half a character to the right on the console. Fix in two steps:
 *   1. the cell pitch: least-squares slope of the CTC centres against the characters'
 *      cell positions (a constant lag does not change the slope);
 *   2. the grid's phase: slide the grid so its cell boundaries fall on the emptiest
 *      columns, i.e. the gaps between glyphs (an ink profile of the line).
 * Returns false (keep the CTC boxes) when the pitch is implausible for the line height,
 * e.g. a proportional font.
 */
#define CELL_MAX_COLS 1280
static bool cell_layout_boxes(SafirTextLine *tl, const uint8_t *gray, uint16_t crop_w,
                              const VocrLine *L, uint16_t crop_h, SafirRect region) {
    const unsigned n = tl->char_count;
    if (n < 2 || L->w < 8 || L->h < 4 || L->w > CELL_MAX_COLS) return false;

    /* 1. pitch from the CTC centres (screen -> crop x) against cell positions in half-cells */
    double sk = 0, sx = 0, skk = 0, skx = 0;
    unsigned halves = 0;
    for (unsigned i = 0; i < n; ++i) {
        unsigned hv = cell_halves(tl->chars[i].codepoint);
        double k = (double)halves + (double)hv / 2.0;
        const SafirRect *b = &tl->chars[i].screen_box;
        double cx = ((double)b->x + (double)b->w / 2.0 - (double)region.x) * (double)crop_w / (double)region.w;
        sk += k; sx += cx; skk += k * k; skx += k * cx;
        halves += hv;
    }
    double den = (double)n * skk - sk * sk;
    if (den <= 0) return false;
    double half = ((double)n * skx - sk * sx) / den;  /* crop px per half-cell */
    double a0 = (sx - half * sk) / (double)n;          /* crop x of half-cell 0 (still lagged) */
    double cell = 2.0 * half;
    if (cell < 0.35 * (double)L->h || cell > 1.6 * (double)L->h) return false;

    /* 2. ink profile of the line, polarity-aware: text pixels are well above (light text)
     *    or below (dark text, L->dark) the line's background. A plain |diff| threshold
     *    also caught box gradients and frame edges. */
    static uint16_t prof[CELL_MAX_COLS];
    uint32_t hist[64] = { 0 };
    for (unsigned y = 0; y < L->h; ++y) {
        const uint8_t *row = gray + (size_t)(L->y + y) * crop_w + L->x;
        for (unsigned x = 0; x < L->w; ++x) hist[row[x] >> 2]++;
    }
    unsigned bg_bin = 0;
    for (unsigned bn = 1; bn < 64; ++bn) if (hist[bn] > hist[bg_bin]) bg_bin = bn;
    const int bg = (int)(bg_bin * 4 + 2);
    const int thr = 64;
    int ink_left = -1, ink_right = -1;
    for (unsigned x = 0; x < L->w; ++x) {
        unsigned ink = 0;
        for (unsigned y = 0; y < L->h; ++y) {
            int v = gray[(size_t)(L->y + y) * crop_w + L->x + x];
            if (L->dark ? (bg - v > thr) : (v - bg > thr)) ink++;
        }
        prof[x] = (uint16_t)ink;
        if (ink >= 2) {
            if (ink_left < 0) ink_left = (int)x;
            ink_right = (int)x;
        }
    }

    double x;
    double span = (ink_left >= 0) ? (double)(ink_right - ink_left + 1) : 0.0;
    double expect = half * (double)halves; /* text width the CTC pitch predicts */
    if (ink_left >= 0 && fabs(span - expect) < cell) {
        /* The ink edges agree with the pitch: lay the cells out between them (glyph side
         * bearings are small, so the ink span is the text width). */
        half = span / (double)halves;
        x = (double)(L->x + (unsigned)ink_left);
    } else {
        /* Otherwise keep the fitted grid (its intercept is where the CTC centres say the
         * first cell starts) and only fine-tune its phase by up to a third of a cell so
         * the boundaries sit on the emptiest columns. A wider search walked onto bright
         * scenery beside the text box on the console screenshot. */
        double best_shift = 0, best_score = 1e30;
        for (double shift = -0.35 * cell; shift <= 0.35 * cell; shift += 0.25) {
            double score = 0;
            for (unsigned k = 0; k <= halves; ++k) {
                int xi = (int)(a0 + shift + half * (double)k - (double)L->x + 0.5);
                score += (xi < 0 || xi >= (int)L->w) ? (double)L->h : (double)prof[xi];
            }
            score += fabs(shift) * 0.5; /* prefer the fit itself among similar phases */
            if (score < best_score) { best_score = score; best_shift = shift; }
        }
        x = a0 + best_shift;
    }
#ifdef SAFIR_DEBUG_CELLS
    fprintf(stderr, "cells: L x=%u w=%u h=%u dark=%u half=%.2f a0=%.2f ink=%d..%d span=%.0f expect=%.0f bg=%d -> x=%.1f\n",
            L->x, L->w, L->h, L->dark, half, a0, ink_left, ink_right, span, expect, bg, x);
    for (unsigned xx = 0; xx < L->w; ++xx) fprintf(stderr, "%c", prof[xx] == 0 ? '.' : (prof[xx] < 4 ? ':' : '#'));
    fprintf(stderr, "\n");
#endif
    for (unsigned i = 0; i < n; ++i) {
        double wdt = half * (double)cell_halves(tl->chars[i].codepoint);
        double x0 = x < (double)L->x ? (double)L->x : x;
        double x1 = x + wdt > (double)(L->x + L->w) ? (double)(L->x + L->w) : x + wdt;
        unsigned cx0 = (unsigned)(x0 + 0.5), cx1 = (unsigned)(x1 + 0.5);
        if (cx1 <= cx0) cx1 = cx0 + 1;
        map_crop_box_to_screen(&tl->chars[i], cx0, L->y, cx1 - cx0, L->h, crop_w, crop_h, region);
        x += wdt;
    }
    return true;
}

/* Turns the greedy CTC path into per-character boxes. Each emitted character is a run
 * of identical non-blank frames; its box spans the midpoints to its neighbours' runs. */
static void emit_char_boxes(const SafirVocrContext *ctx, SafirTextLine *tl, const uint16_t *best,
                            unsigned frames, unsigned resized_w, const VocrLine *L,
                            uint16_t crop_w, uint16_t crop_h, SafirRect region, float conf) {
    unsigned starts[SAFIR_MAX_CHARS_PER_LINE], ends[SAFIR_MAX_CHARS_PER_LINE];
    uint16_t classes[SAFIR_MAX_CHARS_PER_LINE];
    unsigned n = 0;
    for (unsigned f = 0; f < frames && n < SAFIR_MAX_CHARS_PER_LINE; ++f) {
        uint16_t c = best[f];
        if (c == 0 || (f > 0 && best[f - 1] == c)) continue;
        unsigned e = f + 1;
        while (e < frames && best[e] == c) e++;
        starts[n] = f;
        ends[n] = e;
        classes[n] = c;
        n++;
    }
    if (n == 0 || resized_w == 0) return;

    const unsigned stride = ctx->vm.stride;
    /* crop x of frame position p (in frames, may be fractional x2) */
#define FRAME_TO_X(p2) ((unsigned)(L->x + ((uint64_t)(p2) * stride * L->w) / (2ull * resized_w)))
    for (unsigned i = 0; i < n; ++i) {
        unsigned c2 = starts[i] + ends[i];                  /* 2 x run centre */
        unsigned left2, right2;
        if (i > 0) left2 = (c2 + starts[i - 1] + ends[i - 1]) / 2;
        else left2 = (c2 >= (ends[i] - starts[i]) + 2u * stride) ? c2 - ((ends[i] - starts[i]) + 2u * stride) : 0;
        if (i + 1 < n) right2 = (c2 + starts[i + 1] + ends[i + 1]) / 2;
        else right2 = c2 + (ends[i] - starts[i]) + 2u * stride;
        if (right2 > 2u * frames) right2 = 2u * frames;
        if (n == 1) { left2 = 0; right2 = 2u * frames; } /* single character: whole line */

        unsigned x0 = FRAME_TO_X(left2), x1 = FRAME_TO_X(right2);
        if (x0 < L->x) x0 = L->x;
        if (x1 > L->x + L->w) x1 = L->x + L->w;
        if (x1 <= x0) x1 = x0 + 1;

        unsigned len = 0;
        const char *ct = vocr_class_text(&ctx->vm, classes[i], &len);
        /* A class may spell more than one codepoint; split its box evenly. */
        uint32_t cps[8];
        unsigned ncp = 0;
        for (size_t off = 0; ct && off < len && ncp < 8;) {
            size_t k = utf8_decode_one(ct + off, len - off, &cps[ncp]);
            if (k == 0) break;
            off += k;
            ncp++;
        }
        for (unsigned k = 0; k < ncp && tl->char_count < SAFIR_MAX_CHARS_PER_LINE; ++k) {
            unsigned sx0 = x0 + (x1 - x0) * k / ncp;
            unsigned sx1 = x0 + (x1 - x0) * (k + 1) / ncp;
            SafirCharBox *cb = &tl->chars[tl->char_count++];
            cb->codepoint = cps[k];
            cb->confidence = conf;
            map_crop_box_to_screen(cb, sx0, L->y, (sx1 > sx0) ? sx1 - sx0 : 1, L->h, crop_w, crop_h, region);
        }
    }
#undef FRAME_TO_X
}

/* Japanese text, not a HUD or background art: at least one kana/kanji letter, at least two
 * Japanese characters counting Japanese punctuation, and no more Latin/digits than kana and
 * kanji ("221/221", "701Coin", a lone "り" from foliage are dropped; "え？" is kept). */
static bool is_cjk_letter(uint32_t cp) {
    return (cp >= 0x3041 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) || cp == 0x3005 || (cp >= 0xFF66 && cp <= 0xFF9D);
}
static bool is_cjk_punct(uint32_t cp) {
    return (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) ||
           cp == 0x2026 || cp == 0x2025 || cp == 0x2015 || cp == 0x301C;
}
static bool looks_like_japanese(const SafirTextLine *tl) {
    unsigned letters = 0, punct = 0, other = 0;
    for (uint16_t i = 0; i < tl->char_count; ++i) {
        uint32_t cp = tl->chars[i].codepoint;
        if (is_cjk_letter(cp)) letters++;
        else if (is_cjk_punct(cp)) punct++;
        else if (cp > 0x20) other++;
    }
    return letters >= 1 && letters + punct >= 2 && other <= letters;
}
static unsigned cjk_letter_count(const SafirTextLine *tl) {
    unsigned letters = 0;
    for (uint16_t i = 0; i < tl->char_count; ++i) letters += is_cjk_letter(tl->chars[i].codepoint) ? 1u : 0u;
    return letters;
}

static bool s_scan_debug = false; /* host diagnostics (safir_scan_set_debug) */
static const float kMinLineConfidence = 0.6f;
/* Extra lines in a two-pass band (not the band's main line) need clearer evidence: dialogue
 * reads at >= 0.96, while art specks that pass the Japanese test score up to ~0.8. */
static const float kMinExtraLineConfidence = 0.9f;

/* Recognizes line L of the crop and appends it to out (unless filtered). */
static void recognize_one_line(SafirVocrContext *ctx, vocr_ctx *vc, const uint8_t *gray_crop, uint16_t crop_w,
                               uint16_t crop_h, const VocrLine *L, SafirRect screen_region, SafirOcrResult *out_result,
                               bool text_filter, float min_conf) {
    if (L->w < 4 || L->h < 4 || out_result->line_count >= SAFIR_MAX_LINES_PER_BOX) return;
    char text[SAFIR_MAX_CHARS_PER_LINE * 4 + 8];
    uint16_t best[REAL_MAX_FRAMES];
    unsigned frames = 0, rw = 0;
    float conf = 0.0f;
    if (!real_recognize_line(ctx, vc, gray_crop + (size_t)L->y * crop_w + L->x, L->w, L->h, crop_w,
                             (int)L->dark, text, sizeof(text), best, &frames, &rw, &conf)) {
        return;
    }
    if (frames > REAL_MAX_FRAMES) frames = REAL_MAX_FRAMES;
    SafirTextLine *tl = &out_result->lines[out_result->line_count];
    tl->char_count = 0;
    tl->dhash = out_result->dhash;
    emit_char_boxes(ctx, tl, best, frames, rw, L, crop_w, crop_h, screen_region, conf);
    cell_layout_boxes(tl, gray_crop, crop_w, L, crop_h, screen_region);
    /* Dialogue reads at mean CTC confidence >= 0.96 on the fixtures; background art that
     * happens to look like two kana scores ~0.4. */
    /* Junk from HUD digits and art is short ("のウ1り" from "221/221" at 0.745); real short
     * lines (えっ？, はい。) read at >= 0.95. Short lines need the stricter threshold. */
    if (cjk_letter_count(tl) <= 4 && min_conf < kMinExtraLineConfidence) min_conf = kMinExtraLineConfidence;
    if (s_scan_debug) {
        char dbg[SAFIR_MAX_CHARS_PER_LINE * 4 + 8];
        safir_text_line_to_utf8(tl, dbg, sizeof(dbg));
        fprintf(stderr, "  line %ux%u@%u,%u in %ux%u crop: conf %.3f (min %.2f) jp=%d \"%s\"\n", L->w, L->h, L->x,
                L->y, crop_w, crop_h, conf, min_conf, (int)looks_like_japanese(tl), dbg);
    }
    if (tl->char_count > 0 && (!text_filter || (conf >= min_conf && looks_like_japanese(tl)))) {
        out_result->line_count++;
    }
}

static bool real_recognize(SafirVocrContext *ctx, const uint8_t *gray_crop, uint16_t crop_w, uint16_t crop_h,
                           SafirRect screen_region, SafirOcrResult *out_result, bool text_filter) {
    size_t mark = safir_pool_used(POOL_OCR_SCRATCH);

    /* 1. Find dialogue lines (polarity-agnostic; reports dark-on-light per line). */
    VocrLinesImage li;
    li.pixels = gray_crop;
    li.width = crop_w;
    li.height = crop_h;
    li.stride = crop_w;
    li.bpp = VOCR_LINES_GREY;
    size_t lines_need = vocr_lines_arena_bytes(crop_w, crop_h);
    void *lines_arena = safir_alloc(POOL_OCR_SCRATCH, lines_need);
    if (!lines_arena) {
        OCR_ERR("POOL_OCR_SCRATCH OOM: line finder needs %zu B", lines_need);
        return false;
    }
    VocrLineSet set;
    memset(&set, 0, sizeof(set));
    int rc = vocr_find_lines(&li, s_line_finder, lines_arena, lines_need, &set);
    safir_pool_rewind(POOL_OCR_SCRATCH, mark); /* finder arena is scratch only */
    if (rc != VOCR_LINES_OK) {
        OCR_ERR("vocr_find_lines failed (%d)", rc);
        return false;
    }
    if (set.count == 0) {
        OCR_ERR("no text lines found in region");
        return false;
    }

    /* 2. Network arena, valid for this call only. */
    vocr_opts o;
    real_engine_opts(&o);
    size_t need = vocr_arena_bytes(&ctx->vm, &o);
    void *arena = safir_alloc(POOL_OCR_SCRATCH, need);
    if (!arena) {
        OCR_ERR("POOL_OCR_SCRATCH OOM: VOCR arena needs %zu B, free %zu B", need,
                safir_pool_capacity(POOL_OCR_SCRATCH) - safir_pool_used(POOL_OCR_SCRATCH));
        return false;
    }
    vocr_ctx vc;
    rc = vocr_init(&vc, &ctx->vm, &o, arena, need);
    if (rc != VOCR_OK) {
        safir_pool_rewind(POOL_OCR_SCRATCH, mark);
        OCR_ERR("vocr_init: %s", vocr_strerror(rc));
        return false;
    }

    /* 3. Recognize each line, top to bottom. */
    char why[96];
    size_t why_len = 0;
    why[0] = '\0';
    for (unsigned l = 0; l < set.count && out_result->line_count < SAFIR_MAX_LINES_PER_BOX; ++l) {
        const VocrLine *L = &set.lines[l];
        if (why_len + 24 < sizeof(why)) {
            int w = snprintf(why + why_len, sizeof(why) - why_len, "%s%ux%u@%u,%u%s", l ? " " : "",
                             L->w, L->h, L->x, L->y, L->dark ? "d" : "");
            if (w > 0) why_len += (size_t)w;
        }
        recognize_one_line(ctx, &vc, gray_crop, crop_w, crop_h, L, screen_region, out_result, text_filter,
                           kMinLineConfidence);
    }

    safir_pool_rewind(POOL_OCR_SCRATCH, mark);
    if (out_result->line_count == 0) {
        /* line boxes are in 2x-crop pixels; "d" = dark text on a light background */
        OCR_ERR("%u lines found, none decoded: %s", set.count, why);
    }
    return out_result->line_count > 0;
}

static bool vocr_recognize(SafirOcrBackend *self,
                           const uint8_t *gray_crop,
                           uint16_t crop_w,
                           uint16_t crop_h,
                           SafirRect screen_region,
                           SafirOcrResult *out_result) {
    if (!self || !self->userdata || !gray_crop || !out_result || crop_w == 0 || crop_h == 0) {
        return false;
    }
    SafirVocrContext *ctx = (SafirVocrContext *)self->userdata;
    if (!ctx->real) {
        return placeholder_recognize(self, gray_crop, crop_w, crop_h, screen_region, out_result);
    }
    if (ctx->lent) {
        OCR_ERR("model memory is lent out (safir_ocr_vocr_restore_weights not called)");
        return false;
    }

    uint64_t current_dhash = safir_compute_dhash(gray_crop, crop_w, crop_h);
    if (ctx->config.use_dhash_cache && ctx->cache.valid &&
        safir_dhash_distance(current_dhash, ctx->cache.last_dhash) <= SAFIR_DHASH_SIMILAR_THRESHOLD) {
        memcpy(out_result, &ctx->cache.cached_result, sizeof(SafirOcrResult));
        out_result->dhash = current_dhash;
        out_result->cached = true;
        return true;
    }

    memset(out_result, 0, sizeof(SafirOcrResult));
    out_result->dhash = current_dhash;
    bool ok = real_recognize(ctx, gray_crop, crop_w, crop_h, screen_region, out_result, false);
    if (ok) {
        memcpy(&ctx->cache.cached_result, out_result, sizeof(SafirOcrResult));
        ctx->cache.last_dhash = current_dhash;
        ctx->cache.valid = true;
    }
    return ok;
}

SafirOcrBackend *safir_ocr_vocr_create(const SafirOcrConfig *config) {
    SafirOcrBackend *backend = (SafirOcrBackend *)safir_alloc(POOL_OCR_WEIGHTS, sizeof(SafirOcrBackend));
    if (!backend) {
        return NULL;
    }

    SafirVocrContext *ctx = (SafirVocrContext *)safir_alloc(POOL_OCR_WEIGHTS, sizeof(SafirVocrContext));
    if (!ctx) {
        return NULL;
    }

    memset(ctx, 0, sizeof(SafirVocrContext));
    if (config) {
        ctx->config = *config;
    } else {
        ctx->config.decoder_mode = SAFIR_DECODER_GREEDY;
        ctx->config.beam_width = 4;
        ctx->config.min_confidence = 0.5f;
        ctx->config.use_dhash_cache = true;
    }

    backend->name = "vocr_local";
    backend->init = backend_vocr_init;
    backend->recognize = vocr_recognize;
    backend->reset = vocr_reset;
    backend->userdata = ctx;

    return backend;
}

/* ------------------------------------------------------------------------- */
/* Two-pass scan: find lines at half resolution, read text blocks at full    */
/* ------------------------------------------------------------------------- */
/*
 * On a whole-screen scan everything used to run on the 2x-downsampled frame, so 18 px
 * game text became 9 px and VOCR (32 px line height) upscaled it 3.5x: one wrong kanji per
 * line on the console, and the two lines of a small speech box are only ~15 px apart.
 * Now pass 1 only FINDS lines on the half-resolution frame; nearby lines are grouped into
 * text blocks, and pass 2 decodes each block again from the same JPEG at full resolution
 * (when it fits the buffer and the text is small), so recognition sees the real pixels.
 * Needs no network weights, so it runs while the JPEG still occupies the lent model memory.
 */

void safir_scan_set_debug(bool on) { s_scan_debug = on; }

static inline uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }
#define ARRAY_LEN_C(a) (sizeof(a) / sizeof((a)[0]))

static size_t block_bytes(const SafirScanBlock *b) {
    return (size_t)((b->screen.w + b->scale - 1) / b->scale) * (size_t)((b->screen.h + b->scale - 1) / b->scale);
}

int safir_scan_prepare(const uint8_t *jpeg, size_t jpeg_size, SafirRect region, uint8_t *buf, size_t buf_cap,
                       bool half_ready, SafirScanPlan *plan) {
    if (!jpeg || !buf || !plan || region.w < 8 || region.h < 8) return -1;
    memset(plan, 0, sizeof(*plan));
    const uint16_t hw = (uint16_t)((region.w + 1) / 2), hh = (uint16_t)((region.h + 1) / 2);
    if ((size_t)hw * hh > buf_cap) return -2;

    /* Pass 1: half-resolution frame, lines only. */
    if (!half_ready) {
        int rc = safir_jpeg_crop_gray(jpeg, jpeg_size, region, buf, NULL, NULL);
        if (rc != 0) return rc;
    }
    const bool inverted = safir_crop_auto_invert(buf, hw, hh);
    VocrLinesImage li = { buf, hw, hh, hw, VOCR_LINES_GREY };
    size_t mark = safir_pool_used(POOL_OCR_SCRATCH);
    size_t need = vocr_lines_arena_bytes(hw, hh);
    void *arena = safir_alloc(POOL_OCR_SCRATCH, need);
    if (!arena) return -3;
    VocrLineSet set;
    memset(&set, 0, sizeof(set));
    int rc = vocr_find_lines(&li, s_line_finder, arena, need, &set);
    safir_pool_rewind(POOL_OCR_SCRATCH, mark);
    if (rc != VOCR_LINES_OK) return -4;
    plan->pass1_lines = (uint8_t)set.count;
    if (s_scan_debug) {
        for (unsigned i = 0; i < set.count; ++i)
            fprintf(stderr, "  pass1 line %u: %ux%u@%u,%u dark=%d\n", i, set.lines[i].w, set.lines[i].h, set.lines[i].x,
                    set.lines[i].y, set.lines[i].dark);
    }
    if (set.count == 0) return 0;

    /* Pass 1.5: at half resolution a patterned message panel can glue two dialogue lines and
     * the panel's stripes into one box too tall for a line (console 2026-11-05: 640x95, two
     * 30 px lines). Such tall, wide boxes are decoded once more at full resolution (rows
     * trimmed around their centre to fit buf, lowest box first: dialogue sits low), where the
     * finder separates the lines; the lines it finds replace the box. buf is free here: the
     * half-resolution frame is no longer needed. */
    VocrLine lines[VOCR_LINES_MAX * 2];
    unsigned nlines = 0;
    {
        int tall[VOCR_LINES_MAX];
        unsigned ntall = 0;
        for (unsigned i = 0; i < set.count; ++i) {
            const VocrLine *L = &set.lines[i];
            unsigned lh = L->h * 2u;
            /* > 112 px: also the console's 126 px box of that panel, which pass 2 read worse */
            if (lh > 112 && lh <= 400 && L->w >= 3u * L->h) tall[ntall++] = (int)i;
        }
        for (unsigned a = 1; a < ntall; ++a) /* lowest bottom edge first */
            for (unsigned c = a; c > 0 && set.lines[tall[c]].y + set.lines[tall[c]].h >
                                              set.lines[tall[c - 1]].y + set.lines[tall[c - 1]].h; --c) {
                int t = tall[c]; tall[c] = tall[c - 1]; tall[c - 1] = t;
            }
        SafirJpegTarget rt[VOCR_LINES_MAX];
        int rsrc[VOCR_LINES_MAX];
        unsigned nrt = 0;
        size_t used = 0;
        for (unsigned a = 0; a < ntall; ++a) {
            const VocrLine *L = &set.lines[tall[a]];
            int x0 = region.x + (int)L->x * 2 - 8, x1 = region.x + (int)(L->x + L->w) * 2 + 8;
            int y0 = region.y + (int)L->y * 2 - 8, y1 = region.y + (int)(L->y + L->h) * 2 + 8;
            if (x0 < region.x) x0 = region.x;
            if (x1 > region.x + region.w) x1 = region.x + region.w;
            if (y0 < region.y) y0 = region.y;
            if (y1 > region.y + region.h) y1 = region.y + region.h;
            size_t w = (size_t)(x1 - x0), rows = (size_t)(y1 - y0);
            size_t room = buf_cap > used ? (buf_cap - used) / w : 0;
            if (rows > room) { /* trim around the centre */
                if (room < 64) continue;
                int cut = (int)(rows - room);
                y0 += cut / 2;
                y1 = y0 + (int)room;
                rows = room;
            }
            rt[nrt].rect = safir_rect_make((uint16_t)x0, (uint16_t)y0, (uint16_t)w, (uint16_t)rows);
            rt[nrt].scale = 1;
            rt[nrt].out = buf + used;
            rsrc[nrt] = tall[a];
            used += align16((uint32_t)(w * rows));
            nrt++;
        }
        bool replaced[VOCR_LINES_MAX] = { false };
        if (nrt > 0 && safir_jpeg_crop_multi(jpeg, jpeg_size, rt, nrt) == 0) {
            for (unsigned a = 0; a < nrt; ++a) {
                const SafirRect R = rt[a].rect;
                uint8_t *px = rt[a].out;
                if (inverted) /* same polarity as the pass-1 frame */
                    for (size_t q = 0; q < (size_t)R.w * R.h; ++q) px[q] = (uint8_t)(255 - px[q]);
                size_t m2 = safir_pool_used(POOL_OCR_SCRATCH);
                size_t need2 = vocr_lines_arena_bytes(R.w, R.h);
                void *ar2 = safir_alloc(POOL_OCR_SCRATCH, need2);
                if (!ar2) continue;
                VocrLinesImage li2 = { px, R.w, R.h, R.w, VOCR_LINES_GREY };
                VocrLineSet s2;
                memset(&s2, 0, sizeof(s2));
                if (vocr_find_lines(&li2, s_line_finder, ar2, need2, &s2) == VOCR_LINES_OK && s2.count > 0) {
                    replaced[rsrc[a]] = true;
                    for (unsigned k = 0; k < s2.count && nlines < ARRAY_LEN_C(lines); ++k) {
                        const VocrLine *F = &s2.lines[k];
                        if (F->w < 8 || F->h < 8) continue;
                        VocrLine H; /* back to half-resolution units of the region */
                        H.x = (unsigned)((R.x + (int)F->x - region.x) / 2);
                        H.y = (unsigned)((R.y + (int)F->y - region.y) / 2);
                        H.w = (F->w + 1) / 2;
                        H.h = (F->h + 1) / 2;
                        H.dark = F->dark;
                        lines[nlines++] = H;
                        if (s_scan_debug)
                            fprintf(stderr, "  refined line: %ux%u@%u,%u (full res, from pass1 line %d)\n", F->w, F->h,
                                    R.x + F->x, R.y + F->y, rsrc[a]);
                    }
                }
                safir_pool_rewind(POOL_OCR_SCRATCH, m2);
            }
        }
        for (unsigned i = 0; i < set.count && nlines < ARRAY_LEN_C(lines); ++i)
            if (!replaced[i]) lines[nlines++] = set.lines[i];
    }

    /* One band per line (screen pixels), padded a little: the pass-1 box is accurate to a
     * half-resolution pixel. Lines taller than any dialogue line (textbox-sized detections)
     * and slivers are skipped; junk that survives is dropped by the text filter later. */
    const int rx1 = region.x + region.w, ry1 = region.y + region.h;
    for (unsigned i = 0; i < nlines && plan->count < SAFIR_SCAN_MAX_BLOCKS; ++i) {
        const VocrLine *L = &lines[i];
        int lh = (int)L->h * 2;
        if (L->w < 4 || L->h < 4 || lh > 160 || L->w < L->h) continue;
        int padx = lh / 3 > 4 ? lh / 3 : 4, pady = lh / 6 > 2 ? lh / 6 : 2;
        int x0 = region.x + (int)L->x * 2 - padx, y0 = region.y + (int)L->y * 2 - pady;
        int x1 = region.x + (int)(L->x + L->w) * 2 + padx, y1 = region.y + (int)(L->y + L->h) * 2 + pady;
        if (x0 < region.x) x0 = region.x;
        if (y0 < region.y) y0 = region.y;
        if (x1 > rx1) x1 = rx1;
        if (y1 > ry1) y1 = ry1;
        SafirScanBlock *b = &plan->blocks[plan->count++];
        b->screen = safir_rect_make((uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 - x0), (uint16_t)(y1 - y0));
        /* Big text reads fine at half resolution, but a pass-1 box is often taller than its
         * text: small dialogue glued to a portrait or art beside it measured 78 px on the
         * console and was read at half resolution (9 px glyphs, lost). Only really tall boxes
         * go to half resolution; the fit loop below downgrades more if the buffer is short. */
        b->scale = lh < 128 ? 1 : 2;
        b->line_h = (uint16_t)lh;
        b->dark = (uint8_t)((L->dark != 0) != inverted); /* polarity of the real pixels */
    }

    /* Fit the buffer: downgrade the biggest full-resolution band until everything fits
     * (wide background rows go first, dialogue lines usually keep full resolution). */
    for (;;) {
        size_t total = 0;
        for (unsigned j = 0; j < plan->count; ++j) total += align16((uint32_t)block_bytes(&plan->blocks[j]));
        if (total <= buf_cap) break;
        int big = -1;
        for (unsigned j = 0; j < plan->count; ++j) {
            if (plan->blocks[j].scale == 1 && (big < 0 || block_bytes(&plan->blocks[j]) > block_bytes(&plan->blocks[big])))
                big = (int)j;
        }
        if (big >= 0) plan->blocks[big].scale = 2;
        else if (plan->count > 0) plan->count--;
        else break;
    }

    /* Pass 2: decode every band in one JPEG pass (overwrites the half-resolution frame). */
    SafirJpegTarget t[SAFIR_SCAN_MAX_BLOCKS];
    uint32_t off = 0;
    for (unsigned j = 0; j < plan->count; ++j) {
        SafirScanBlock *b = &plan->blocks[j];
        b->offset = off;
        b->w = (uint16_t)((b->screen.w + b->scale - 1) / b->scale);
        b->h = (uint16_t)((b->screen.h + b->scale - 1) / b->scale);
        t[j].rect = b->screen;
        t[j].scale = b->scale;
        t[j].out = buf + off;
        off += align16((uint32_t)block_bytes(b));
    }
    if (plan->count == 0) return 0;
    rc = safir_jpeg_crop_multi(jpeg, jpeg_size, t, plan->count);
    if (rc != 0) {
        plan->count = 0;
        return rc;
    }
    return 0;
}

/* Screen-pixel bounding box of a recognized line's characters. */
static SafirRect line_screen_rect(const SafirTextLine *tl) {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (uint16_t i = 0; i < tl->char_count; ++i) {
        const SafirRect *r = &tl->chars[i].screen_box;
        if (i == 0 || r->x < x0) x0 = r->x;
        if (i == 0 || r->y < y0) y0 = r->y;
        if (i == 0 || r->x + r->w > x1) x1 = r->x + r->w;
        if (i == 0 || r->y + r->h > y1) y1 = r->y + r->h;
    }
    return safir_rect_make((uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 - x0), (uint16_t)(y1 - y0));
}

/*
 * Splits a found line at wide empty column gaps. The line finder can glue dialogue to the
 * character portrait beside it (console 2026-11-05: a 383x46 box from the portrait's frame
 * to the end of line 2). The glued box is too tall, so VOCR shrinks the 18 px glyphs, and the
 * frame reads as "「。". Ink is judged against the line's own background (median vs the
 * brightest/darkest 1%), so dim art behind a translucent box does not count. Gaps between
 * characters are a few px; a gap of half the box height or more separates pieces. Each piece
 * is re-found with the line finder (training-style box at its own height). Returns the
 * number of pieces written to out (0 = no split: read L as it is).
 */
static unsigned split_line_at_gaps(const uint8_t *px, uint16_t stride, const VocrLine *L, VocrLine *out, unsigned max) {
    if (L->w < 16 || L->h < 8 || max == 0) return 0;
    uint32_t hist[256];
    memset(hist, 0, sizeof(hist));
    for (unsigned y = 0; y < L->h; ++y) {
        const uint8_t *row = px + (size_t)(L->y + y) * stride + L->x;
        for (unsigned x = 0; x < L->w; ++x) hist[row[x]]++;
    }
    const uint32_t total = (uint32_t)L->w * L->h;
    int median = 0, ext = 0;
    uint32_t acc = 0;
    for (int v = 0; v < 256; ++v) { acc += hist[v]; if (acc * 2 >= total) { median = v; break; } }
    acc = 0;
    if (!L->dark) { for (int v = 255; v >= 0; --v) { acc += hist[v]; if (acc * 100 >= total) { ext = v; break; } } }
    else          { for (int v = 0; v < 256; ++v)  { acc += hist[v]; if (acc * 100 >= total) { ext = v; break; } } }
    int contrast = L->dark ? median - ext : ext - median;
    if (contrast < 32) return 0;
    const int thr = (median + ext) / 2;

    /* Column ink, then runs of ink columns joined across gaps narrower than gap_min. */
    const unsigned gap_min = L->h / 2 > 8 ? L->h / 2 : 8;
    unsigned seg0[8], seg1[8], nseg = 0;
    int last_ink = -1, start = -1;
    for (unsigned x = 0; x <= L->w; ++x) {
        unsigned ink = 0;
        if (x < L->w) {
            for (unsigned y = 0; y < L->h && ink < 2; ++y) {
                int p = px[(size_t)(L->y + y) * stride + L->x + x];
                if (L->dark ? p < thr : p > thr) ink++;
            }
        }
        if (ink >= 2) {
            if (start < 0) start = (int)x;
            else if ((int)x - last_ink > (int)gap_min) {
                if (nseg < 8) { seg0[nseg] = (unsigned)start; seg1[nseg] = (unsigned)last_ink; nseg++; }
                start = (int)x;
            }
            last_ink = (int)x;
        } else if (x == L->w && start >= 0) {
            if (nseg < 8) { seg0[nseg] = (unsigned)start; seg1[nseg] = (unsigned)last_ink; nseg++; }
        }
    }
    if (nseg < 2) return 0;

    /* Ink rows of each run. A comma or full stop leaves a gap of up to ~1 character after
     * it, so a gap alone is not enough: split only where it is >= 1.5x the text height, or
     * where the two sides have clearly different ink heights (a portrait beside text). */
    unsigned top[8], bot[8];
    for (unsigned s = 0; s < nseg; ++s) {
        top[s] = L->h; bot[s] = 0;
        for (unsigned y = 0; y < L->h; ++y) {
            const uint8_t *row = px + (size_t)(L->y + y) * stride + L->x;
            for (unsigned x = seg0[s]; x <= seg1[s]; ++x) {
                int p = row[x];
                if (L->dark ? p < thr : p > thr) { if (y < top[s]) top[s] = y; if (y > bot[s]) bot[s] = y; break; }
            }
        }
    }
    unsigned m = 0; /* merged run count */
    for (unsigned s = 1; s < nseg; ++s) {
        unsigned ha = bot[m] >= top[m] ? bot[m] - top[m] + 1 : 1, hb = bot[s] >= top[s] ? bot[s] - top[s] + 1 : 1;
        unsigned hmin = ha < hb ? ha : hb, hmax = ha < hb ? hb : ha;
        unsigned gap = seg0[s] - seg1[m] - 1;
        bool split = gap * 2 >= hmin * 3 || hmax * 10 > hmin * 14;
        if (split) {
            ++m;
            seg0[m] = seg0[s]; seg1[m] = seg1[s]; top[m] = top[s]; bot[m] = bot[s];
        } else {
            seg1[m] = seg1[s];
            if (top[s] < top[m]) top[m] = top[s];
            if (bot[s] > bot[m]) bot[m] = bot[s];
        }
    }
    nseg = m + 1;
    if (nseg < 2) return 0;

    unsigned n = 0;
    for (unsigned s = 0; s < nseg && n < max; ++s) {
        unsigned pad = L->h / 4;
        unsigned x0 = seg0[s] > pad ? seg0[s] - pad : 0;
        unsigned x1 = seg1[s] + 1 + pad < L->w ? seg1[s] + 1 + pad : L->w;
        if (x1 - x0 < 4) continue;
        VocrLine piece = { L->x + x0, L->y, x1 - x0, L->h, L->dark };
        size_t mark = safir_pool_used(POOL_OCR_SCRATCH);
        size_t need = vocr_lines_arena_bytes(piece.w, piece.h);
        void *arena = safir_alloc(POOL_OCR_SCRATCH, need);
        if (arena) {
            VocrLinesImage li = { px + (size_t)piece.y * stride + piece.x, piece.w, piece.h, stride, VOCR_LINES_GREY };
            VocrLineSet set;
            memset(&set, 0, sizeof(set));
            if (vocr_find_lines(&li, s_line_finder, arena, need, &set) == VOCR_LINES_OK) {
                int widest = -1;
                for (unsigned k = 0; k < set.count; ++k)
                    if (set.lines[k].w >= 4 && set.lines[k].h >= 4 && (widest < 0 || set.lines[k].w > set.lines[widest].w))
                        widest = (int)k;
                if (widest >= 0) {
                    const VocrLine *c = &set.lines[widest];
                    piece.x += c->x;
                    piece.y += c->y;
                    piece.w = c->w;
                    piece.h = c->h;
                }
            }
        }
        safir_pool_rewind(POOL_OCR_SCRATCH, mark);
        out[n++] = piece;
    }
    return n;
}

/* recognize_one_line, after splitting a glued line into its pieces. */
static void recognize_split(SafirVocrContext *ctx, vocr_ctx *vc, const uint8_t *px, uint16_t w, uint16_t h,
                            const VocrLine *L, SafirRect screen, SafirOcrResult *out, float min_conf) {
    VocrLine pieces[4];
    unsigned n = split_line_at_gaps(px, w, L, pieces, 4);
    if (s_scan_debug && n) fprintf(stderr, "  split %ux%u@%u,%u into %u pieces\n", L->w, L->h, L->x, L->y, n);
    if (n == 0) { recognize_one_line(ctx, vc, px, w, h, L, screen, out, true, min_conf); return; }
    for (unsigned k = 0; k < n && out->line_count < SAFIR_MAX_LINES_PER_BOX; ++k)
        recognize_one_line(ctx, vc, px, w, h, &pieces[k], screen, out, true, min_conf);
}

/* True when the overlap covers more than half of the smaller box. */
static bool rect_mostly_overlaps(SafirRect a, SafirRect b) {
    int ix = (a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w) - (a.x > b.x ? a.x : b.x);
    int iy = (a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h) - (a.y > b.y ? a.y : b.y);
    if (ix <= 0 || iy <= 0) return false;
    long inter = (long)ix * iy, sa = (long)a.w * a.h, sb = (long)b.w * b.h;
    long smaller = sa < sb ? sa : sb;
    return smaller > 0 && inter * 2 > smaller;
}

bool safir_ocr_vocr_recognize_plan(SafirOcrBackend *backend, const uint8_t *buf, const SafirScanPlan *plan,
                                   SafirOcrResult *out) {
    if (!backend || !backend->userdata || !buf || !plan || !out) return false;
    SafirVocrContext *ctx = (SafirVocrContext *)backend->userdata;
    memset(out, 0, sizeof(*out));
    if (!ctx->real) { OCR_ERR("two-pass scan needs the real model"); return false; }
    if (ctx->lent) { OCR_ERR("model memory is lent out (safir_ocr_vocr_restore_weights not called)"); return false; }
    if (plan->count == 0) {
        OCR_ERR("no text lines found (%u candidate lines)", (unsigned)plan->pass1_lines);
        return false;
    }
    vocr_opts o;
    real_engine_opts(&o);
    size_t need = vocr_arena_bytes(&ctx->vm, &o);
    size_t mark = safir_pool_used(POOL_OCR_SCRATCH);
    void *arena = safir_alloc(POOL_OCR_SCRATCH, need);
    if (!arena) {
        OCR_ERR("POOL_OCR_SCRATCH OOM: VOCR arena needs %zu B", need);
        return false;
    }
    vocr_ctx vc;
    if (vocr_init(&vc, &ctx->vm, &o, arena, need) != VOCR_OK) {
        safir_pool_rewind(POOL_OCR_SCRATCH, mark);
        OCR_ERR("vocr_init failed");
        return false;
    }
    /* Each band holds a pass-1 line box plus a margin. Find lines again at the band's own
     * resolution (precise, training-style crop boxes). The main line is the one nearest the
     * band's centre (fallback: the whole band). Other lines lying fully inside the band are
     * read too, with a stricter filter: pass 1 can glue a dialogue line to the portrait or
     * art beside it, and then the band's centre is the art (console 2026-11-05: line 2 of
     * a speech box was lost this way). Lines clipped by the band edge belong to a neighbour. */
    for (unsigned j = 0; j < plan->count && out->line_count < SAFIR_MAX_LINES_PER_BOX; ++j) {
        const SafirScanBlock *b = &plan->blocks[j];
        const uint8_t *px = buf + b->offset;
        VocrLine cand[VOCR_LINES_MAX];
        unsigned ncand = 0;
        size_t fmark = safir_pool_used(POOL_OCR_SCRATCH);
        size_t fneed = vocr_lines_arena_bytes(b->w, b->h);
        void *farena = safir_alloc(POOL_OCR_SCRATCH, fneed);
        if (farena) {
            VocrLinesImage li = { px, b->w, b->h, b->w, VOCR_LINES_GREY };
            VocrLineSet set;
            memset(&set, 0, sizeof(set));
            if (vocr_find_lines(&li, s_line_finder, farena, fneed, &set) == VOCR_LINES_OK) {
                for (unsigned k = 0; k < set.count; ++k)
                    if (set.lines[k].w >= 4 && set.lines[k].h >= 4) cand[ncand++] = set.lines[k];
            }
        }
        safir_pool_rewind(POOL_OCR_SCRATCH, fmark);

        const int cy = (int)b->h / 2;
        int best = -1, best_d = 0;
        for (unsigned k = 0; k < ncand; ++k) {
            int mid = (int)(cand[k].y + cand[k].h / 2);
            int d = mid > cy ? mid - cy : cy - mid;
            if (best < 0 || d < best_d) { best = (int)k; best_d = d; }
        }
        VocrLine L;
        if (best >= 0 && best_d * 3 <= (int)b->h) {
            L = cand[best];
        } else {
            best = -1;
            memset(&L, 0, sizeof(L));
            L.w = b->w;
            L.h = b->h;
            L.dark = b->dark;
        }
        recognize_split(ctx, &vc, px, b->w, b->h, &L, b->screen, out, kMinLineConfidence);

        for (unsigned k = 0; k < ncand && out->line_count < SAFIR_MAX_LINES_PER_BOX; ++k) {
            if ((int)k == best) continue;
            const VocrLine *c = &cand[k];
            if (c->y <= 1 || (int)(c->y + c->h) >= (int)b->h - 1) continue;                   /* clipped */
            /* Overlaps the main line. Finder boxes are generous (63 px for 30 px glyphs), so the
             * boxes of two adjacent lines can share a few rows (console 2026-11-05: line 1 of a
             * two-line panel was skipped over a 6-row overlap); only a real overlap counts. */
            if (best >= 0) {
                int ov = (int)(c->y + c->h < L.y + L.h ? c->y + c->h : L.y + L.h) - (int)(c->y > L.y ? c->y : L.y);
                int hmin = (int)(c->h < L.h ? c->h : L.h);
                if (ov * 3 > hmin) continue;
            }
            recognize_split(ctx, &vc, px, b->w, b->h, c, b->screen, out, kMinExtraLineConfidence);
        }
    }
    safir_pool_rewind(POOL_OCR_SCRATCH, mark);

    /* Bands overlap: drop a line that mostly covers one already kept, then order top to
     * bottom (extra lines can come out of order). */
    {
        SafirRect box[SAFIR_MAX_LINES_PER_BOX];
        for (unsigned l = 0; l < out->line_count; ++l) box[l] = line_screen_rect(&out->lines[l]);
        unsigned kept = 0;
        for (unsigned l = 0; l < out->line_count; ++l) {
            bool dup = false;
            for (unsigned m = 0; m < kept && !dup; ++m) dup = rect_mostly_overlaps(box[l], box[m]);
            if (dup) continue;
            if (kept != l) { out->lines[kept] = out->lines[l]; box[kept] = box[l]; }
            kept++;
        }
        out->line_count = (uint16_t)kept;
        for (unsigned l = 1; l < out->line_count; ++l) {
            for (unsigned m = l; m > 0 && box[m].y < box[m - 1].y; --m) {
                SafirTextLine t = out->lines[m]; out->lines[m] = out->lines[m - 1]; out->lines[m - 1] = t;
                SafirRect r = box[m]; box[m] = box[m - 1]; box[m - 1] = r;
            }
        }
    }
    if (out->line_count == 0) {
        OCR_ERR("%u blocks, no Japanese text in them", (unsigned)plan->count);
        return false;
    }
    return true;
}
