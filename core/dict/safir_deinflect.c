/*
 * core/dict/safir_deinflect.c
 *
 * Japanese deinflection with Yomitan's transform table (safir_deinflect_rules.h, generated
 * from tools/yomitan_transforms.json), walked the way hoshidicts' Deinflector does it:
 *
 *   recurse(text, conditions):
 *     a form of one character is dropped; otherwise it is a result
 *     for each suffix of text, longest (up to SAFIR_DEINFLECT_MAX_SUFFIX_CHARS) first:
 *       for each rule with that suffix, in Yomitan's order:
 *         skip it unless text is the surface (conditions 0) or conditions & rule.in
 *         recurse(text - suffix + rule.out, rule.out conditions)
 *
 *   行ってしまいました -た-> 行ってしまいます -ます-> 行ってしまう -しまう-> 行って -て-> 行く
 *
 * One difference, which changes no result: a form reached again with the same conditions is
 * not expanded a second time (its first trace is the one Yomitan keeps too).
 */
#include "safir_deinflect.h"
#include <string.h>

#include "safir_deinflect_rules.h"

#define RULE_COUNT (sizeof(s_rules) / sizeof(s_rules[0]))
#define NAME_COUNT (sizeof(s_rule_names) / sizeof(s_rule_names[0]))

const char *safir_rule_name(unsigned id) { return id < NAME_COUNT ? s_rule_names[id] : ""; }

/* What a dictionary entry must be for a form with these conditions (hoshidicts'
 * pos_to_conditions, the other way round). The surface (0) matches anything. */
static uint16_t conds_to_pos(uint32_t conds) {
    if (conds == 0) return SAFIR_POS_ALL;
    uint16_t pos = 0;
    if (conds & C_V1) pos |= SAFIR_POS_VERB_ICHIDAN;
    if (conds & C_V5) pos |= SAFIR_POS_VERB_GODAN;
    if (conds & (C_VS | C_VZ)) pos |= SAFIR_POS_VERB_SURU;  /* the packer files vz under suru */
    if (conds & C_VK) pos |= SAFIR_POS_VERB_KURU;
    if (conds & C_ADJI) pos |= SAFIR_POS_ADJ_I;
    return pos;  /* 0: an intermediate form (-て, -ます...), never a dictionary word */
}

static size_t utf8_chars(const char *s, size_t bytes) {
    size_t n = 0;
    for (size_t i = 0; i < bytes; ++i)
        if ((((unsigned char)s[i]) & 0xC0) != 0x80) ++n;
    return n;
}

/* Byte offset of the start of the last `chars` characters of s[0..bytes). */
static size_t tail_start(const char *s, size_t bytes, size_t chars) {
    size_t i = bytes;
    while (i > 0 && chars > 0) {
        --i;
        if ((((unsigned char)s[i]) & 0xC0) != 0x80) --chars;
    }
    return i;
}

/* First rule whose suffix is >= key (rules are sorted by suffix bytes). */
static size_t lower_bound(const char *key, size_t len) {
    size_t lo = 0, hi = RULE_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const DeinflectRule *r = &s_rules[mid];
        size_t n = r->in_len < len ? r->in_len : len;
        int c = memcmp(r->in, key, n);
        if (c < 0 || (c == 0 && r->in_len < len)) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

typedef struct Walk {
    SafirDeinflectCandidate *out;
    size_t count, cap;
} Walk;

static bool seen(const Walk *w, const char *term, uint32_t conds) {
    for (size_t i = 0; i < w->count; ++i)
        if (w->out[i].conds == conds && strcmp(w->out[i].term, term) == 0) return true;
    return false;
}

/* trace: Yomitan order (outermost rule first), stored base first in rule_trace. */
static void set_trace(SafirDeinflectCandidate *c, const uint8_t *trace, size_t depth) {
    size_t n = 0;
    c->rule_trace[0] = '\0';
    for (size_t k = depth; k-- > 0;) {
        const char *name = safir_rule_name(trace[k]);
        size_t len = strlen(name);
        if (n + len + 2 > sizeof(c->rule_trace)) break;  /* cut at a name boundary */
        if (n > 0) c->rule_trace[n++] = '+';
        memcpy(c->rule_trace + n, name, len);
        n += len;
        c->rule_trace[n] = '\0';
    }
}

static void recurse(Walk *w, const char *text, uint32_t conds, uint8_t *trace, size_t depth) {
    const size_t bytes = strlen(text);
    const size_t chars = utf8_chars(text, bytes);
    if (chars <= 1 || w->count >= w->cap || depth > SAFIR_DEINFLECT_MAX_STEPS) return;
    if (depth > 0 && seen(w, text, conds)) return;

    SafirDeinflectCandidate *c = &w->out[w->count++];
    memcpy(c->term, text, bytes + 1);
    c->conds = conds;
    c->pos_mask = depth == 0 ? SAFIR_POS_ALL : conds_to_pos(conds);
    c->steps = (uint8_t)depth;
    set_trace(c, trace, depth);
    if (depth == SAFIR_DEINFLECT_MAX_STEPS) return;

    const size_t longest = chars < SAFIR_DEINFLECT_MAX_SUFFIX_CHARS ? chars : SAFIR_DEINFLECT_MAX_SUFFIX_CHARS;
    for (size_t k = longest; k > 0; --k) {
        const size_t at = tail_start(text, bytes, k);
        const char *suffix = text + at;
        const size_t slen = bytes - at;
        for (size_t r = lower_bound(suffix, slen); r < RULE_COUNT; ++r) {
            const DeinflectRule *rule = &s_rules[r];
            if (rule->in_len != slen || memcmp(rule->in, suffix, slen) != 0) break;
            if (conds != 0 && !(conds & rule->cond_in)) continue;
            if (at + rule->out_len >= sizeof(c->term)) continue;
            char next[sizeof(c->term)];
            memcpy(next, text, at);
            memcpy(next + at, rule->out, rule->out_len);
            next[at + rule->out_len] = '\0';
            trace[depth] = rule->name;
            recurse(w, next, rule->cond_out, trace, depth + 1);
        }
    }
}

size_t safir_deinflect(const char *surface, SafirDeinflectCandidate *out, size_t max_candidates) {
    if (!surface || !out || max_candidates == 0) return 0;
    char text[sizeof(out[0].term)];
    size_t len = strlen(surface);
    if (len >= sizeof(text)) {
        len = sizeof(text) - 1;
        while (len > 0 && (((unsigned char)surface[len]) & 0xC0) == 0x80) len--;  /* whole characters */
    }
    memcpy(text, surface, len);
    text[len] = '\0';
    if (len == 0) return 0;

    if (utf8_chars(text, len) <= 1) {  /* hoshidicts: a single character is only itself */
        memcpy(out[0].term, text, len + 1);
        out[0].conds = 0;
        out[0].pos_mask = SAFIR_POS_ALL;
        out[0].steps = 0;
        out[0].rule_trace[0] = '\0';
        return 1;
    }
    Walk w = {out, 0, max_candidates};
    uint8_t trace[SAFIR_DEINFLECT_MAX_STEPS + 1];
    recurse(&w, text, 0, trace, 0);
    return w.count;
}
