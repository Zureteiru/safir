#ifndef SAFIR_LEDGER_H
#define SAFIR_LEDGER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Memory Ledger Static Pool Budgets (total 1,240,000 bytes; on the Switch POOL_OCR_WEIGHTS is attached, so 480,000 B of .bss).
 *
 * POOL_OCR_WEIGHTS holds the whole VOCR6.2_S_w8.vocr blob, which the real recognizer
 * (libs/vocr/vocr.c) reads in place: 819,792 B blob + ~22 KB backend/context state.
 * The earlier 2345-0002 failures happened at 2.25-4.5 MB of .bss; a 584 KB build booted.
 */
#define SAFIR_POOL_OCR_WEIGHTS_MAX_BYTES    760000ULL /* host only: a streamed model (Normal 728 KB without its head) + backend; on the Switch it is attached */
#define SAFIR_POOL_OCR_SCRATCH_MAX_BYTES    320000ULL /* 320 KB: textspot8 arena (305,247 B for the 640x360 pass-1 frame; a band's up to ~210 KB beside the 80 KB VOCR arena) */
#define SAFIR_POOL_DICT_INDEX_MAX_BYTES     100000ULL /* 100 KB: Level-1 sparse anchors + block table when it fits */
#define SAFIR_POOL_UI_STATE_MAX_BYTES        60000ULL /* 60 KB: 32 KB L2 page + 16 KB glossary block */

#define SAFIR_TOTAL_BUDGET_MAX_BYTES       1240000ULL /* 1.24 MB total */

typedef enum SafirPool {
    POOL_OCR_WEIGHTS = 0,
    POOL_OCR_SCRATCH = 1,
    POOL_DICT_INDEX  = 2,
    POOL_UI_STATE    = 3,
    POOL_COUNT       = 4
} SafirPool;

typedef struct SafirPoolStats {
    const char *name;
    size_t capacity;
    size_t used;
    size_t high_watermark;
    size_t alloc_count;
} SafirPoolStats;

/**
 * Initializes the static memory ledger arenas and resets all tracking offsets.
 */
void safir_ledger_init(void);

/**
 * Allocates `bytes` from the specified pool arena with 8-byte alignment.
 * Returns NULL if the allocation exceeds pool capacity or if the pool is invalid.
 */
void *safir_alloc(SafirPool pool, size_t bytes);

/**
 * Resets the allocation offset of the specified pool back to zero.
 */
void safir_pool_reset(SafirPool pool);

/**
 * Releases everything allocated from `pool` after `mark` (a value previously
 * returned by safir_pool_used()). Used for temporary per-call allocations.
 */
void safir_pool_rewind(SafirPool pool, size_t mark);

/**
 * Backs `pool` with `buf` (cap bytes, 16-byte aligned) instead of its static arena, and
 * empties it. Lets the Switch overlay size POOL_OCR_WEIGHTS to the model it loads (one heap
 * block taken at start-up) instead of reserving the largest model in .bss. NULL/0 goes back
 * to the static arena. Survives safir_ledger_init(). Returns false on a misaligned buffer.
 * With SAFIR_OCR_WEIGHTS_EXTERNAL defined (Switch build) the static weights arena is empty,
 * so the overlay must attach memory before loading a model.
 */
bool safir_pool_attach(SafirPool pool, void *buf, size_t cap);

/**
 * Dumps memory pool ledger statistics (used, capacity, high-watermark) to stdout.
 */
void safir_ledger_dump_stats(void);

/**
 * Query stats for an individual pool.
 */
bool safir_ledger_get_stats(SafirPool pool, SafirPoolStats *out_stats);

/**
 * Returns current bytes used in a given pool.
 */
size_t safir_pool_used(SafirPool pool);

/**
 * Returns capacity in bytes for a given pool.
 */
size_t safir_pool_capacity(SafirPool pool);

/**
 * Returns total static bytes allocated across all pools.
 */
size_t safir_ledger_total_used(void);

/**
 * Returns total static budget across all pools.
 */
size_t safir_ledger_total_capacity(void);

#ifdef __cplusplus
}
#endif

#endif /* SAFIR_LEDGER_H */
