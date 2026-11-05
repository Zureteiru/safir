/*
 * core/layout/safir_glyph_cache.c: fixed-size glyph cache (see include/safir_glyph_cache.h).
 */
#include "safir_glyph_cache.h"
#include <string.h>

void safir_glyph_cache_init(SafirGlyphCache *c, SafirGlyph *slots, uint32_t slot_count, uint8_t *arena, size_t arena_bytes) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    uint32_t n = 1;
    while (n * 2 <= slot_count) n *= 2;
    c->slots = slots;
    c->slot_count = slots ? n : 0;
    c->arena = arena;
    c->arena_bytes = arena ? arena_bytes : 0;
    safir_glyph_cache_reset(c);
    c->resets = 0;
}

void safir_glyph_cache_reset(SafirGlyphCache *c) {
    if (!c) return;
    if (c->slots) memset(c->slots, 0, (size_t)c->slot_count * sizeof(SafirGlyph));
    c->used = 0;
    c->arena_used = 0;
    c->reset_pending = false;
    c->resets++;
}

void safir_glyph_cache_begin(SafirGlyphCache *c) {
    if (c && c->reset_pending) safir_glyph_cache_reset(c);
}

static uint32_t hash_key(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    return (uint32_t)k;
}

SafirGlyph *safir_glyph_cache_get(SafirGlyphCache *c, uint64_t key, bool *is_new) {
    if (is_new) *is_new = false;
    if (!c || !c->slots || c->slot_count == 0 || key == 0) return NULL;
    /* Keep the table at most 3/4 full so probes stay short. */
    if ((c->used + 1) * 4 > c->slot_count * 3) safir_glyph_cache_reset(c);
    const uint32_t mask = c->slot_count - 1;
    for (uint32_t i = hash_key(key) & mask, n = 0; n < c->slot_count; i = (i + 1) & mask, ++n) {
        SafirGlyph *g = &c->slots[i];
        if (g->key == key) return g;
        if (g->key == 0) {
            memset(g, 0, sizeof(*g));
            g->key = key;
            c->used++;
            if (is_new) *is_new = true;
            return g;
        }
    }
    return NULL; /* unreachable: the table is never full */
}

uint8_t *safir_glyph_cache_bitmap(SafirGlyphCache *c, SafirGlyph *g, uint16_t w, uint16_t h) {
    if (!c || !g) return NULL;
    size_t need = ((size_t)w * h + 7u) & ~(size_t)7u;
    if (need == 0) return NULL;
    if (c->arena_used + need > c->arena_bytes) {
        c->reset_pending = true;
        return NULL;
    }
    uint8_t *p = c->arena + c->arena_used;
    c->arena_used += need;
    g->bitmap = p;
    g->w = w;
    g->h = h;
    return p;
}
