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
 * exist to continue a chain. 96 x 128 B = 12 KB. */
#define SAFIR_MAX_DEINFLECT_CANDIDATES 96
#define SAFIR_DEINFLECT_MAX_STEPS      7

/* Word-class conditions a rule reads and produces (tools/gen_deinflect_rules.py). */
#define C_V1    (1u << 0)   /* ichidan verb */
#define C_V5    (1u << 1)   /* godan verb */
#define C_VS    (1u << 2)   /* suru verb */
#define C_VK    (1u << 3)   /* kuru verb */
#define C_VZ    (1u << 4)   /* zuru verb (感ずる) */
#define C_ADJI  (1u << 5)   /* i-adjective */
#define C_MASU  (1u << 6)   /* -ます */
#define C_MASEN (1u << 7)   /* -ません */
#define C_TE    (1u << 8)   /* -て form */
#define C_BA    (1u << 9)   /* -ば form */
#define C_TA    (1u << 10)  /* -た form */
#define C_SOU   (1u << 11)  /* -そう */
#define C_SURF  (1u << 15)  /* text as it appears on screen: only the start of a chain */
#define C_DICT_FORMS (C_V1 | C_V5 | C_VS | C_VK | C_VZ | C_ADJI)

typedef struct DeinflectRule {
    const char *in;
    const char *out;
    uint8_t in_len;
    uint8_t out_len;
    uint16_t cond_in;
    uint16_t cond_out;
    uint8_t name;
} DeinflectRule;

typedef struct SafirDeinflectCandidate {
    char term[64];             /* Reconstructed form (UTF-8) */
    uint16_t pos_mask;         /* POS the dictionary entry must have; 0 = intermediate form, not a word */
    uint16_t conds;            /* C_* conditions of this form */
    uint8_t steps;             /* rules applied (0 = the surface itself) */
    char rule_trace[48];       /* base-first, e.g. "causative+passive+negative+past" */
} SafirDeinflectCandidate;

/**
 * Generates deinflection candidates from an inflected Japanese surface string.
 * Candidate 0 is always the unaltered surface string with SAFIR_POS_ALL. Candidates come
 * in breadth-first order, so fewer steps come first. Returns the number written.
 */
size_t safir_deinflect(const char* surface, SafirDeinflectCandidate* out_candidates, size_t max_candidates);

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
