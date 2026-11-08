#ifndef SAFIR_DEINFLECT_H
#define SAFIR_DEINFLECT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "safir_dict.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Candidates per surface string, including intermediate forms (pos_mask 0) that only
 * exist to continue a chain. At most 31 came up over the 200k inputs of the
 * tools/dict_parity check; 128 x ~200 B = ~25 KB, kept static by the caller. */
#define SAFIR_MAX_DEINFLECT_CANDIDATES 128
#define SAFIR_DEINFLECT_MAX_STEPS      16   /* depth guard; Yomitan's conditions end chains well before */
#define SAFIR_RULE_TRACE_BYTES         128

/* Word-class conditions a rule reads and produces: Yomitan's, with hoshidicts' bit values
 * (tools/yomitan_transforms.json). A form with conditions 0 is the text as it appears on
 * screen: every rule may start there, and rules whose conditions in are 0 apply only there. */
#define C_V1D   (1u << 0)   /* ichidan verb */
#define C_V1P   (1u << 1)   /* ichidan verb, progressive / perfect (-ている stem) */
#define C_V5D   (1u << 2)   /* godan verb */
#define C_V5SS  (1u << 3)   /* godan verb, special (いく / 行く...) */
#define C_V5SP  (1u << 4)   /* godan verb, special (こう / 問う...) */
#define C_VK    (1u << 5)   /* kuru verb */
#define C_VS    (1u << 6)   /* suru verb */
#define C_VZ    (1u << 7)   /* zuru verb (感ずる) */
#define C_ADJI  (1u << 8)   /* i-adjective */
#define C_MASU  (1u << 9)   /* -ます */
#define C_MASEN (1u << 10)  /* -ません */
#define C_TE    (1u << 11)  /* -て */
#define C_BA    (1u << 12)  /* -ば */
#define C_KU    (1u << 13)  /* -く */
#define C_TA    (1u << 14)  /* -た */
#define C_NN    (1u << 15)  /* -ん */
#define C_NASAI (1u << 16)  /* -なさい */
#define C_YA    (1u << 17)  /* -ゃ */
#define C_V1    (C_V1D | C_V1P)
#define C_V5    (C_V5D | C_V5SS | C_V5SP)

typedef struct DeinflectRule {
    const char *in;
    const char *out;
    uint8_t in_len;
    uint8_t out_len;
    uint32_t cond_in;
    uint32_t cond_out;
    uint8_t name;
} DeinflectRule;

typedef struct SafirDeinflectCandidate {
    char term[64];             /* Reconstructed form (UTF-8) */
    uint16_t pos_mask;         /* POS the dictionary entry must have; 0 = intermediate form, not a word */
    uint32_t conds;            /* C_* conditions of this form (0 = the surface text) */
    uint8_t steps;             /* rules applied (0 = the surface itself) */
    char rule_trace[SAFIR_RULE_TRACE_BYTES];  /* Yomitan group names, base first: "-て+-しまう+-ます+-た" */
} SafirDeinflectCandidate;

/**
 * Generates deinflection candidates from an inflected Japanese surface string, the way
 * Yomitan / hoshidicts do: depth-first, longest suffix first, rules in Yomitan's order, and
 * no form of one character (a one-character surface yields only itself). Candidate 0 is the
 * unaltered surface with SAFIR_POS_ALL. A form reached again with the same conditions keeps
 * its first trace and is not expanded twice. Returns the number written.
 */
size_t safir_deinflect(const char* surface, SafirDeinflectCandidate* out_candidates, size_t max_candidates);

/* Name of rule group `id` (Yomitan's), or "" when out of range. */
const char *safir_rule_name(unsigned id);

/**
 * Checks if candidate POS matches dictionary entry POS mask.
 */
static inline bool safir_pos_matches(uint16_t entry_pos, uint16_t required_pos) {
    if (required_pos == SAFIR_POS_ALL) return true;
    return (entry_pos & required_pos) != 0;
}

#ifdef __cplusplus
}
#endif

#endif /* SAFIR_DEINFLECT_H */
