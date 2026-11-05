#ifndef SAFIR_GLYPH_CACHE_H
#define SAFIR_GLYPH_CACHE_H

/*
 * Fixed-size glyph cache for the overlay's text drawing (used by libtesla's drawString).
 *
 * libtesla's own cache was a std::unordered_map holding a malloc'd bitmap per
 * (character, size) forever: in a long VN session it grew until the 4 MB overlay heap ran
 * out (missing letters on the console, then an abort inside the map). This cache owns two
 * buffers handed to it once at start-up: a slot table and a bump arena for bitmaps. When
 * either is full it starts over (the next frames re-rasterise what they draw). It never
 * allocates and never fails.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SafirGlyph {
    uint64_t key;        /* codepoint << 32 | size/style bits; 0 = empty slot */
    const void *font;    /* font chosen for the codepoint (opaque to the cache) */
    float scale;
    int16_t x0, y0;      /* bitmap offset from the pen position */
    uint16_t w, h;       /* bitmap size */
    int32_t advance;     /* unscaled advance width */
    uint8_t *bitmap;     /* 8-bit coverage, w*h bytes, in the arena; NULL until rendered */
    uint8_t rendered;    /* bitmap made (or the glyph has no pixels) */
} SafirGlyph;

typedef struct SafirGlyphCache {
    SafirGlyph *slots;
    uint32_t slot_count; /* power of two */
    uint32_t used;
    uint8_t *arena;
    size_t arena_bytes;
    size_t arena_used;
    uint32_t resets;     /* times the cache started over (diagnostics) */
    bool reset_pending;
} SafirGlyphCache;

/* slot_count is rounded down to a power of two. */
void safir_glyph_cache_init(SafirGlyphCache *c, SafirGlyph *slots, uint32_t slot_count, uint8_t *arena, size_t arena_bytes);
void safir_glyph_cache_reset(SafirGlyphCache *c);

/* Call once per drawString before any lookup: performs a reset requested while drawing
 * (glyph pointers stay valid for the whole string). */
void safir_glyph_cache_begin(SafirGlyphCache *c);

/* Returns the slot for key; *is_new says it was just created (fill in the metrics).
 * If the table is too full, the cache resets first (pointers from before are invalid). */
SafirGlyph *safir_glyph_cache_get(SafirGlyphCache *c, uint64_t key, bool *is_new);

/* Bitmap memory for a slot; NULL when the arena is full (a reset is then scheduled for
 * the next safir_glyph_cache_begin, and the glyph is drawn next frame). */
uint8_t *safir_glyph_cache_bitmap(SafirGlyphCache *c, SafirGlyph *g, uint16_t w, uint16_t h);

#ifdef __cplusplus
}
#endif

#endif /* SAFIR_GLYPH_CACHE_H */
