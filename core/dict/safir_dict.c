#include "safir_dict.h"
#include "safir_deinflect.h"
#include "safir_inflate.h"
#include "safir_ledger.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* Helper: converts a UTF-32 codepoint to UTF-8 bytes */
static size_t utf32_to_utf8(uint32_t cp, char *out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* Helper: encodes a slice of SafirCharBox into a null-terminated UTF-8 string */
static size_t chars_to_utf8(const SafirCharBox *chars, uint16_t count, char *out, size_t max_out) {
    size_t out_idx = 0;
    for (uint16_t i = 0; i < count; ++i) {
        char buf[4];
        size_t n = utf32_to_utf8(chars[i].codepoint, buf);
        if (out_idx + n >= max_out) break;
        for (size_t b = 0; b < n; ++b) {
            out[out_idx++] = buf[b];
        }
    }
    out[out_idx] = '\0';
    return out_idx;
}

static bool ensure_block_cached(SafirDict *dict, uint16_t block_id) {
    if (!dict || !dict->cached_block_buffer) return false;
    if (dict->cached_block_id == (int32_t)block_id) return true;
    if (block_id >= dict->header.block_count) return false;

    SafirBlockMeta meta_val;
    const SafirBlockMeta *meta = NULL;
    if (dict->block_metas) {
        meta = &dict->block_metas[block_id];
    } else if (dict->file_handle) {
        long offset = (long)dict->header.blocks_offset + (long)block_id * (long)sizeof(SafirBlockMeta);
        if (fseek(dict->file_handle, offset, SEEK_SET) != 0) return false;
        if (fread(&meta_val, sizeof(SafirBlockMeta), 1, dict->file_handle) != 1) return false;
        meta = &meta_val;
    } else {
        return false;
    }

    if (dict->raw_file_memory) {
        if (meta->file_offset + meta->comp_size > dict->raw_file_size) return false;
        const uint8_t *src = dict->raw_file_memory + meta->file_offset;

        if (dict->header.compression_type == SAFIR_COMPRESS_DEFLATE) {
            size_t written = safir_inflate(dict->cached_block_buffer, dict->header.block_size, src, meta->comp_size);
            if (written != meta->uncomp_size) return false;
        } else {
            memcpy(dict->cached_block_buffer, src, meta->uncomp_size);
        }
    } else if (dict->file_handle) {
        uint8_t comp_buf[SAFIR_DICT_BLOCK_SIZE * 2];
        if (meta->comp_size > sizeof(comp_buf)) return false;

        if (fseek(dict->file_handle, (long)meta->file_offset, SEEK_SET) != 0) return false;
        if (fread(comp_buf, 1, meta->comp_size, dict->file_handle) != meta->comp_size) return false;

        if (dict->header.compression_type == SAFIR_COMPRESS_DEFLATE) {
            size_t written = safir_inflate(dict->cached_block_buffer, dict->header.block_size, comp_buf, meta->comp_size);
            if (written != meta->uncomp_size) return false;
        } else {
            memcpy(dict->cached_block_buffer, comp_buf, meta->uncomp_size);
        }
    } else {
        return false;
    }

    dict->cached_block_id = (int32_t)block_id;
    return true;
}

static void init_prefix_table(SafirDict *dict) {
    if (!dict || !dict->l1_anchors || dict->l1_anchor_count == 0) return;
    size_t total = dict->l1_anchor_count;
    size_t cur = 0;

    for (uint32_t b = 0; b < 256; ++b) {
        while (cur < total) {
            uint8_t first_byte = (uint8_t)dict->l1_anchors[cur].headword[0];
            if (first_byte >= b) {
                break;
            }
            cur++;
        }
        dict->prefix_table[b] = (uint32_t)cur;
    }
}

/* Returns the Level-2 page of anchor_idx, from the single page buffer or the optional LRU. */
static const uint8_t *load_l2_page(SafirDict *dict, uint32_t anchor_idx) {
    if (!dict || !dict->cached_l2_buffer || anchor_idx >= dict->l1_anchor_count) return NULL;
    if (dict->cached_l2_anchor_id == (int32_t)anchor_idx) return dict->cached_l2_buffer;

    uint8_t *dst = dict->cached_l2_buffer;
    int slot = -1;
    if (dict->page_cache && dict->page_cache_slots > 0) {
        uint32_t oldest = UINT32_MAX;
        for (int i = 0; i < dict->page_cache_slots; ++i) {
            if (dict->page_cache_anchor[i] == (int32_t)anchor_idx) {
                dict->page_cache_age[i] = ++dict->page_cache_clock;
                return dict->page_cache + (size_t)i * SAFIR_L2_PAGE_SIZE;
            }
            if (dict->page_cache_age[i] < oldest) {
                oldest = dict->page_cache_age[i];
                slot = i;
            }
        }
        dst = dict->page_cache + (size_t)slot * SAFIR_L2_PAGE_SIZE;
        dict->page_cache_anchor[slot] = -1; /* invalid until the read succeeds */
    }

    const SafirL1Anchor *anchor = &dict->l1_anchors[anchor_idx];
    if (anchor->page_bytes > SAFIR_L2_PAGE_SIZE) return NULL;

    if (dict->raw_file_memory) {
        if (anchor->file_offset + anchor->page_bytes > dict->raw_file_size) return NULL;
        memcpy(dst, dict->raw_file_memory + anchor->file_offset, anchor->page_bytes);
    } else if (dict->file_handle) {
        if (fseek(dict->file_handle, (long)anchor->file_offset, SEEK_SET) != 0) return NULL;
        if (fread(dst, 1, anchor->page_bytes, dict->file_handle) != anchor->page_bytes) return NULL;
    } else {
        return NULL;
    }
    dict->page_reads++;

    if (slot >= 0) {
        dict->page_cache_anchor[slot] = (int32_t)anchor_idx;
        dict->page_cache_age[slot] = ++dict->page_cache_clock;
    } else {
        dict->cached_l2_anchor_id = (int32_t)anchor_idx;
    }
    return dst;
}

typedef void (*EntryVisitor)(void *ctx, const SafirDiskIndexEntry *entry);

/* Calls visit() for every index entry whose key is exactly term and whose POS matches.
 * A key with many entries (は has 30+ reading-key rows) can span several pages: the
 * anchor search finds the LAST page starting at or before term, so walk back over pages
 * that start with term too. The old code only looked at that page and the next one, so
 * it could miss the first (best) entries of a long run: は -> 破 instead of the particle. */
static void visit_key(SafirDict *dict, const char *term, uint16_t pos_filter, EntryVisitor visit, void *ctx) {
    if (!dict || !dict->l1_anchors || dict->l1_anchor_count == 0 || !term || term[0] == '\0') return;

    int l = 0, r = (int)dict->l1_anchor_count - 1, target = -1;
    while (l <= r) {
        int mid = l + ((r - l) >> 1);
        if (strcmp(dict->l1_anchors[mid].headword, term) <= 0) {
            target = mid;
            l = mid + 1;
        } else {
            r = mid - 1;
        }
    }
    if (target < 0) return;

    int first = target;
    while (first > 0 && strcmp(dict->l1_anchors[first].headword, term) == 0) first--;
    /* The next page too: anchors store at most 43 bytes of a headword, so a long key can
     * sort past a truncated anchor. */
    int last = target + 1 < (int)dict->l1_anchor_count ? target + 1 : target;

    for (int a = first; a <= last; ++a) {
        const uint8_t *page = load_l2_page(dict, (uint32_t)a);
        if (!page) continue;
        const SafirL2PageHeader *hdr = (const SafirL2PageHeader *)page;
        const SafirDiskIndexEntry *entries = (const SafirDiskIndexEntry *)(page + sizeof(SafirL2PageHeader));
        const char *str_tbl = (const char *)(page + sizeof(SafirL2PageHeader) +
                                             (size_t)hdr->entry_count * sizeof(SafirDiskIndexEntry));
        /* lower_bound */
        int lo = 0, hi = (int)hdr->entry_count;
        while (lo < hi) {
            int mid = lo + ((hi - lo) >> 1);
            if (strcmp(str_tbl + entries[mid].term_offset, term) < 0) lo = mid + 1;
            else hi = mid;
        }
        for (int i = lo; i < (int)hdr->entry_count; ++i) {
            if (strcmp(str_tbl + entries[i].term_offset, term) != 0) break;
            if (!safir_pos_matches(entries[i].pos_mask, pos_filter)) continue;
            SafirDiskIndexEntry e = entries[i]; /* copy: the page buffer may be reused */
            visit(ctx, &e);
        }
    }
}

static inline uint32_t rank_of(uint16_t freq_rank) {
    return freq_rank ? freq_rank : 0x10000u; /* 0 = unranked: last */
}

typedef struct BestCtx {
    SafirDiskIndexEntry best;
    bool found;
} BestCtx;

static void visit_best(void *ctx, const SafirDiskIndexEntry *e) {
    BestCtx *b = (BestCtx *)ctx;
    if (!b->found || rank_of(e->freq_rank) < rank_of(b->best.freq_rank)) {
        b->best = *e;
        b->found = true;
    }
}

/* Best-ranked entry for term: the lowest non-zero freq_rank. Reading-only keys carry
 * +32768, so written-form matches win. */
static bool search_entry(SafirDict *dict, const char *term, uint16_t pos_filter, SafirDiskIndexEntry *out_entry) {
    if (!out_entry) return false;
    BestCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    visit_key(dict, term, pos_filter, visit_best, &ctx);
    if (ctx.found) *out_entry = ctx.best;
    return ctx.found;
}

static bool fill_lookup_result(SafirDict *dict, const SafirDiskIndexEntry *entry, const char *rule_trace,
                               uint16_t matched_chars, uint16_t matched_bytes, SafirLookupResult *out) {
    if (!ensure_block_cached(dict, entry->block_id)) return false;

    const char *block_data = (const char *)dict->cached_block_buffer;
    const char *hw = block_data + entry->offset_in_block;
    size_t hw_len = strlen(hw);
    const char *rd = hw + hw_len + 1;
    size_t rd_len = strlen(rd);
    const char *gl = rd + rd_len + 1;

    strncpy(out->headword, hw, sizeof(out->headword) - 1);
    out->headword[sizeof(out->headword) - 1] = '\0';

    strncpy(out->reading, rd, sizeof(out->reading) - 1);
    out->reading[sizeof(out->reading) - 1] = '\0';

    strncpy(out->glossary, gl, sizeof(out->glossary) - 1);
    out->glossary[sizeof(out->glossary) - 1] = '\0';
    if (strlen(gl) >= sizeof(out->glossary) - 1) {
        /* Merged multi-sense glossaries can exceed the buffer: never end mid-character. */
        size_t n = sizeof(out->glossary) - 1;
        while (n > 0 && (((unsigned char)out->glossary[n - 1]) & 0xC0) == 0x80) n--;
        if (n > 0 && ((unsigned char)out->glossary[n - 1]) >= 0xC0) n--;
        out->glossary[n] = '\0';
    }

    if (rule_trace && rule_trace[0] != '\0') {
        strncpy(out->deinflect_rule, rule_trace, sizeof(out->deinflect_rule) - 1);
        out->deinflect_rule[sizeof(out->deinflect_rule) - 1] = '\0';
    } else {
        out->deinflect_rule[0] = '\0';
    }

    out->matched_chars = matched_chars;
    out->matched_bytes = matched_bytes;
    out->pos_mask = entry->pos_mask;
    out->pitch_mask = entry->pitch_mask;
    out->freq_rank = entry->freq_rank;
    out->found = true;

    return true;
}

static char s_dict_last_error[160] = "no load attempted";

const char *safir_dict_last_error(void) {
    return s_dict_last_error;
}

#define DICT_FAIL(...) do { \
        snprintf(s_dict_last_error, sizeof(s_dict_last_error), __VA_ARGS__); \
        if (f) fclose(f); \
        dict->file_handle = NULL; \
        return false; \
    } while (0)

bool safir_dict_load_file(SafirDict *dict, const char *filepath) {
    if (!dict || !filepath) {
        snprintf(s_dict_last_error, sizeof(s_dict_last_error), "bad argument");
        return false;
    }
    memset(dict, 0, sizeof(*dict));
    dict->cached_block_id = -1;
    dict->cached_l2_anchor_id = -1;

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        int e = errno;
        snprintf(s_dict_last_error, sizeof(s_dict_last_error), "fopen failed errno=%d (%s)", e, strerror(e));
        return false;
    }
    dict->file_handle = f;

    if (fread(&dict->header, sizeof(dict->header), 1, f) != 1)
        DICT_FAIL("file too short for 64-byte header");

    if (dict->header.magic != SAFIR_DICT_MAGIC)
        DICT_FAIL("magic mismatch: 0x%08X (want VNPD 0x%08X)", (unsigned)dict->header.magic, (unsigned)SAFIR_DICT_MAGIC);
    if (dict->header.version != SAFIR_DICT_VERSION)
        DICT_FAIL("version %u unsupported (want %u)", (unsigned)dict->header.version, (unsigned)SAFIR_DICT_VERSION);
    if (dict->header.block_size == 0 || dict->header.block_size > SAFIR_DICT_BLOCK_SIZE)
        DICT_FAIL("block_size %u invalid (max %u)", (unsigned)dict->header.block_size, (unsigned)SAFIR_DICT_BLOCK_SIZE);

    /* 1. Level-1 anchors are REQUIRED: allocate them first so the optional
     *    block table can never starve them (audit: the old order did exactly that
     *    with the 38 MB JMdict pack: 72 KB block table + 78 KB anchors > 100 KB pool). */
    size_t anchors_bytes = (size_t)dict->header.l1_anchor_count * sizeof(SafirL1Anchor);
    if (dict->header.l1_anchor_count == 0)
        DICT_FAIL("dictionary has 0 L1 anchors");
    SafirL1Anchor *anchors = (SafirL1Anchor *)safir_alloc(POOL_DICT_INDEX, anchors_bytes);
    if (!anchors)
        DICT_FAIL("POOL_DICT_INDEX OOM: anchors need %zu B, free %zu of %zu B",
                  anchors_bytes, safir_pool_capacity(POOL_DICT_INDEX) - safir_pool_used(POOL_DICT_INDEX),
                  safir_pool_capacity(POOL_DICT_INDEX));
    if (fseek(f, (long)dict->header.l1_anchors_offset, SEEK_SET) != 0 ||
        fread(anchors, sizeof(SafirL1Anchor), dict->header.l1_anchor_count, f) != dict->header.l1_anchor_count)
        DICT_FAIL("read error in L1 anchors (truncated file?)");
    for (uint32_t i = 0; i < dict->header.l1_anchor_count; ++i) {
        if (anchors[i].page_bytes > SAFIR_L2_PAGE_SIZE)
            DICT_FAIL("L2 page %u is %u B > reader max %u B; repack", (unsigned)i,
                      (unsigned)anchors[i].page_bytes, (unsigned)SAFIR_L2_PAGE_SIZE);
    }
    dict->l1_anchors = anchors;
    dict->l1_anchor_count = dict->header.l1_anchor_count;

    /* 2. Block metadata is OPTIONAL: keep it in RAM if it fits, otherwise
     *    ensure_block_cached() reads each 8-byte entry from disk on demand. */
    size_t block_metas_bytes = (size_t)dict->header.block_count * sizeof(SafirBlockMeta);
    SafirBlockMeta *bmetas = NULL;
    if (block_metas_bytes > 0 &&
        block_metas_bytes <= safir_pool_capacity(POOL_DICT_INDEX) - safir_pool_used(POOL_DICT_INDEX)) {
        bmetas = (SafirBlockMeta *)safir_alloc(POOL_DICT_INDEX, block_metas_bytes);
    }
    if (bmetas) {
        if (fseek(f, (long)dict->header.blocks_offset, SEEK_SET) != 0 ||
            fread(bmetas, sizeof(SafirBlockMeta), dict->header.block_count, f) != dict->header.block_count)
            DICT_FAIL("read error in block table");
    }
    dict->block_metas = bmetas;

    /* 3. Level-2 page buffer in POOL_UI_STATE */
    dict->cached_l2_buffer = (uint8_t *)safir_alloc(POOL_UI_STATE, SAFIR_L2_PAGE_SIZE);
    if (!dict->cached_l2_buffer)
        DICT_FAIL("POOL_UI_STATE OOM: L2 page buffer needs %u B", (unsigned)SAFIR_L2_PAGE_SIZE);

    /* 4. Uncompressed glossary block cache in POOL_UI_STATE */
    dict->cached_block_buffer = (uint8_t *)safir_alloc(POOL_UI_STATE, dict->header.block_size);
    if (!dict->cached_block_buffer)
        DICT_FAIL("POOL_UI_STATE OOM: block cache needs %u B", (unsigned)dict->header.block_size);

    init_prefix_table(dict);
    snprintf(s_dict_last_error, sizeof(s_dict_last_error), "ok: %u entries, %u anchors, block table %s",
             (unsigned)dict->header.entry_count, (unsigned)dict->l1_anchor_count, bmetas ? "in RAM" : "on SD");
    return true;
}

bool safir_dict_load_memory(SafirDict *dict, const void *data, size_t size) {
    if (!dict || !data || size < sizeof(SafirDictHeader)) return false;
    memset(dict, 0, sizeof(*dict));
    dict->cached_block_id = -1;
    dict->cached_l2_anchor_id = -1;

    memcpy(&dict->header, data, sizeof(SafirDictHeader));
    if (dict->header.magic != SAFIR_DICT_MAGIC || dict->header.version != SAFIR_DICT_VERSION) {
        return false;
    }

    dict->raw_file_memory = (const uint8_t *)data;
    dict->raw_file_size = size;

    /* 1. Allocate and copy block metadata into POOL_DICT_INDEX */
    size_t block_metas_bytes = dict->header.block_count * sizeof(SafirBlockMeta);
    if (dict->header.blocks_offset + block_metas_bytes > size) return false;
    SafirBlockMeta *bmetas = (SafirBlockMeta *)safir_alloc(POOL_DICT_INDEX, block_metas_bytes);
    if (!bmetas && block_metas_bytes > 0) return false;
    memcpy(bmetas, dict->raw_file_memory + dict->header.blocks_offset, block_metas_bytes);
    dict->block_metas = bmetas;

    /* 2. Allocate and copy Level-1 anchors into POOL_DICT_INDEX */
    size_t anchors_bytes = dict->header.l1_anchor_count * sizeof(SafirL1Anchor);
    if (dict->header.l1_anchors_offset + anchors_bytes > size) return false;
    SafirL1Anchor *anchors = (SafirL1Anchor *)safir_alloc(POOL_DICT_INDEX, anchors_bytes);
    if (!anchors && anchors_bytes > 0) return false;
    memcpy(anchors, dict->raw_file_memory + dict->header.l1_anchors_offset, anchors_bytes);
    dict->l1_anchors = anchors;
    dict->l1_anchor_count = dict->header.l1_anchor_count;

    /* 3. Allocate 4 KB Level-2 page buffer in POOL_UI_STATE */
    dict->cached_l2_buffer = (uint8_t *)safir_alloc(POOL_UI_STATE, SAFIR_L2_PAGE_SIZE);
    if (!dict->cached_l2_buffer) return false;

    /* 4. Allocate 16 KB uncompressed glossary block cache in POOL_UI_STATE */
    dict->cached_block_buffer = (uint8_t *)safir_alloc(POOL_UI_STATE, dict->header.block_size);
    if (!dict->cached_block_buffer) return false;

    init_prefix_table(dict);
    return true;
}

void safir_dict_close(SafirDict *dict) {
    if (!dict) return;
    if (dict->file_handle) {
        fclose(dict->file_handle);
        dict->file_handle = NULL;
    }
    dict->raw_file_memory = NULL;
    dict->raw_file_size = 0;
    dict->l1_anchors = NULL;
    dict->l1_anchor_count = 0;
    dict->block_metas = NULL;
    dict->cached_l2_buffer = NULL;
    dict->cached_l2_anchor_id = -1;
    dict->cached_block_buffer = NULL;
    dict->cached_block_id = -1;
}

bool safir_dict_lookup_exact(SafirDict *dict, const char *term, uint16_t pos_filter, SafirLookupResult *out) {
    if (!dict || !term || !out) return false;
    memset(out, 0, sizeof(*out));

    SafirDiskIndexEntry entry;
    if (!search_entry(dict, term, pos_filter, &entry)) {
        return false;
    }

    return fill_lookup_result(dict, &entry, "", (uint16_t)strlen(term), (uint16_t)strlen(term), out);
}

/* ------------------------------------------------------------------------- */
/* Scanning: every entry for text at a position                              */
/* ------------------------------------------------------------------------- */

/* Characters a dictionary word never spans: CJK and full-width punctuation, ASCII
 * punctuation and spaces. Stopping the scan window there saves lookups that cannot hit. */
static bool is_break_char(uint32_t cp) {
    if (cp <= 0x2F || (cp >= 0x3A && cp <= 0x40) || (cp >= 0x5B && cp <= 0x60) || (cp >= 0x7B && cp <= 0x7F)) return true;
    if (cp >= 0x3000 && cp <= 0x303F && cp != 0x3005 /* 々 */ && cp != 0x3006 && cp != 0x3007) return true;
    if (cp >= 0xFF01 && cp <= 0xFF0F) return true; /* ！＂＃... */
    if (cp >= 0xFF1A && cp <= 0xFF20) return true; /* ：；＜＝＞？＠ */
    if (cp == 0x2026 || cp == 0x2025 || cp == 0x2015 || cp == 0x2014 || cp == 0x30FB /* ・ */) return true;
    return false;
}

/* Katakana -> hiragana, so カタカナ-written verbs and okurigana still hit (Yomitan does the
 * same). Returns false when nothing changed. */
static bool katakana_to_hiragana(const SafirCharBox *chars, uint16_t count, char *out, size_t cap) {
    bool changed = false;
    size_t n = 0;
    for (uint16_t i = 0; i < count; ++i) {
        uint32_t cp = chars[i].codepoint;
        if (cp >= 0x30A1 && cp <= 0x30F6) {
            cp -= 0x60;
            changed = true;
        }
        char buf[4];
        size_t k = utf32_to_utf8(cp, buf);
        if (n + k >= cap) break;
        memcpy(out + n, buf, k);
        n += k;
    }
    out[n] = '\0';
    return changed;
}

/* Work buffers: lookups run on one thread at a time (safir_engine_lock on Switch).
 * Static rather than on the stack: ~12 KB of candidates per call. */
static SafirDeinflectCandidate s_cands[SAFIR_MAX_DEINFLECT_CANDIDATES];
#define SAFIR_LOOKUP_SCRATCH 64
static SafirDictMatch s_matches[SAFIR_LOOKUP_SCRATCH];

typedef struct CollectCtx {
    SafirDictMatch *buf;
    size_t count, cap;
    uint16_t matched_chars, matched_bytes;
    const SafirDeinflectCandidate *cand;
} CollectCtx;

static void visit_collect(void *ctx, const SafirDiskIndexEntry *e) {
    CollectCtx *c = (CollectCtx *)ctx;
    for (size_t i = 0; i < c->count; ++i) { /* same entry through another path: keep the first */
        if (c->buf[i].block_id == e->block_id && c->buf[i].offset_in_block == e->offset_in_block) return;
    }
    if (c->count >= c->cap) return;
    SafirDictMatch *m = &c->buf[c->count++];
    memset(m, 0, sizeof(*m));
    m->block_id = e->block_id;
    m->offset_in_block = e->offset_in_block;
    m->gloss_len = e->gloss_len;
    m->pos_mask = e->pos_mask;
    m->freq_rank = e->freq_rank;
    m->matched_chars = c->matched_chars;
    m->matched_bytes = c->matched_bytes;
    m->steps = c->cand->steps;
    memcpy(m->rule, c->cand->rule_trace, sizeof(m->rule));
    m->rule[sizeof(m->rule) - 1] = '\0';
}

static void collect_surface(SafirDict *dict, const char *surface, CollectCtx *ctx) {
    size_t n = safir_deinflect(surface, s_cands, SAFIR_MAX_DEINFLECT_CANDIDATES);
    for (size_t c = 0; c < n && ctx->count < ctx->cap; ++c) {
        if (s_cands[c].pos_mask == 0) continue; /* intermediate form, not a word */
        ctx->cand = &s_cands[c];
        visit_key(dict, s_cands[c].term, s_cands[c].pos_mask, visit_collect, ctx);
    }
}

/* All entries for the first len characters at start (original text, then its hiragana form). */
static void collect_len(SafirDict *dict, const SafirTextLine *line, uint16_t start, uint16_t len, CollectCtx *ctx) {
    char surface[128];
    size_t bytes = chars_to_utf8(&line->chars[start], len, surface, sizeof(surface));
    if (bytes == 0) return;
    ctx->matched_chars = len;
    ctx->matched_bytes = (uint16_t)bytes;
    collect_surface(dict, surface, ctx);
    char hira[128];
    if (katakana_to_hiragana(&line->chars[start], len, hira, sizeof(hira))) collect_surface(dict, hira, ctx);
}

static uint16_t scan_window(const SafirTextLine *line, uint16_t start) {
    uint16_t available = (uint16_t)(line->char_count - start);
    uint16_t max_scan = available > 16 ? 16 : available;
    for (uint16_t k = 0; k < max_scan; ++k) {
        if (is_break_char(line->chars[start + k].codepoint)) return k;
    }
    return max_scan;
}

/* Yomitan order: longer source text, then fewer deinflection steps, then more common. */
static bool match_before(const SafirDictMatch *a, const SafirDictMatch *b) {
    if (a->matched_chars != b->matched_chars) return a->matched_chars > b->matched_chars;
    if (a->steps != b->steps) return a->steps < b->steps;
    return rank_of(a->freq_rank) < rank_of(b->freq_rank);
}

static void sort_matches(SafirDictMatch *m, size_t n) { /* stable insertion sort, n <= 64 */
    for (size_t i = 1; i < n; ++i) {
        SafirDictMatch key = m[i];
        size_t j = i;
        while (j > 0 && match_before(&key, &m[j - 1])) {
            m[j] = m[j - 1];
            --j;
        }
        m[j] = key;
    }
}

size_t safir_dict_lookup_all(SafirDict *dict, const SafirTextLine *line, uint16_t start_char_idx,
                             SafirDictMatch *out, size_t max_out) {
    if (!dict || !line || !out || max_out == 0 || start_char_idx >= line->char_count) return 0;
    CollectCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buf = s_matches;
    ctx.cap = SAFIR_LOOKUP_SCRATCH;
    for (uint16_t len = scan_window(line, start_char_idx); len >= 1 && ctx.count < ctx.cap; --len) {
        collect_len(dict, line, start_char_idx, len, &ctx);
    }
    sort_matches(s_matches, ctx.count);
    size_t n = ctx.count < max_out ? ctx.count : max_out;
    memcpy(out, s_matches, n * sizeof(SafirDictMatch));
    return n;
}

static void copy_utf8_cap(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && (((unsigned char)src[n]) & 0xC0) == 0x80) n--; /* src[n] starts a char */
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool safir_dict_read_entry(SafirDict *dict, const SafirDictMatch *m, char *headword, size_t headword_cap,
                           char *reading, size_t reading_cap, char *glossary, size_t glossary_cap) {
    if (!dict || !m || !ensure_block_cached(dict, m->block_id)) return false;
    if (m->offset_in_block >= dict->header.block_size) return false;
    const char *hw = (const char *)dict->cached_block_buffer + m->offset_in_block;
    const char *rd = hw + strlen(hw) + 1;
    const char *gl = rd + strlen(rd) + 1;
    copy_utf8_cap(headword, headword_cap, hw);
    copy_utf8_cap(reading, reading_cap, rd);
    copy_utf8_cap(glossary, glossary_cap, gl);
    return true;
}

void safir_dict_set_page_cache(SafirDict *dict, void *buf, size_t bytes) {
    if (!dict) return;
    size_t slots = buf ? bytes / SAFIR_L2_PAGE_SIZE : 0;
    if (slots > SAFIR_DICT_MAX_CACHE_PAGES) slots = SAFIR_DICT_MAX_CACHE_PAGES;
    dict->page_cache = slots ? (uint8_t *)buf : NULL;
    dict->page_cache_slots = (uint8_t)slots;
    for (int i = 0; i < SAFIR_DICT_MAX_CACHE_PAGES; ++i) {
        dict->page_cache_anchor[i] = -1;
        dict->page_cache_age[i] = 0;
    }
    dict->page_cache_clock = 0;
}

uint32_t safir_dict_page_reads(const SafirDict *dict) {
    return dict ? dict->page_reads : 0;
}

/* Longest match at start_char_idx; among entries of that length the one with the fewest
 * deinflection steps, then the most common (とした: とする "to try to" before 賭する). */
bool safir_dict_scan_longest(SafirDict *dict, const SafirTextLine *line, uint16_t start_char_idx, SafirLookupResult *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!dict || !line || start_char_idx >= line->char_count || !out) return false;

    CollectCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.buf = s_matches;
    ctx.cap = SAFIR_LOOKUP_SCRATCH;
    for (uint16_t len = scan_window(line, start_char_idx); len >= 1; --len) {
        ctx.count = 0;
        collect_len(dict, line, start_char_idx, len, &ctx);
        if (ctx.count == 0) continue;
        sort_matches(s_matches, ctx.count);
        const SafirDictMatch *m = &s_matches[0];
        SafirDiskIndexEntry e;
        memset(&e, 0, sizeof(e));
        e.block_id = m->block_id;
        e.offset_in_block = m->offset_in_block;
        e.gloss_len = m->gloss_len;
        e.pos_mask = m->pos_mask;
        e.freq_rank = m->freq_rank;
        return fill_lookup_result(dict, &e, m->rule, m->matched_chars, m->matched_bytes, out);
    }
    return false;
}

void safir_dict_tokenize_line(SafirDict *dict, const SafirTextLine *line, SafirLineTokens *out_tokens) {
    if (!line || !out_tokens) return;
    out_tokens->token_count = 0;
    if (!dict || line->char_count == 0) return;

    uint16_t idx = 0;
    SafirLookupResult match;

    while (idx < line->char_count && out_tokens->token_count < SAFIR_MAX_TOKENS_PER_LINE) {
        if (safir_dict_scan_longest(dict, line, idx, &match) && match.matched_chars > 0) {
            uint16_t t = out_tokens->token_count++;
            out_tokens->tokens[t].start_char_idx = idx;
            out_tokens->tokens[t].char_count = match.matched_chars;
            out_tokens->tokens[t].is_dict_word = true;
            idx += match.matched_chars;
        } else {
            uint16_t t = out_tokens->token_count++;
            out_tokens->tokens[t].start_char_idx = idx;
            out_tokens->tokens[t].char_count = 1;
            out_tokens->tokens[t].is_dict_word = false;
            idx += 1;
        }
    }
}

int safir_tokens_next(const SafirLineTokens *tokens, uint16_t cur_char_idx) {
    if (!tokens || tokens->token_count == 0) return -1;
    for (uint16_t i = 0; i < tokens->token_count; ++i) {
        if (tokens->tokens[i].start_char_idx > cur_char_idx) {
            return (int)tokens->tokens[i].start_char_idx;
        }
    }
    return -1;
}

int safir_tokens_prev(const SafirLineTokens *tokens, uint16_t cur_char_idx) {
    if (!tokens || tokens->token_count == 0) return -1;
    for (int i = (int)tokens->token_count - 1; i >= 0; --i) {
        if (tokens->tokens[i].start_char_idx < cur_char_idx) {
            return (int)tokens->tokens[i].start_char_idx;
        }
    }
    return -1;
}
