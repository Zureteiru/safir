#include "safir_ledger.h"
#include <stdio.h>
#include <string.h>

#if defined(__cplusplus)
  #define SAFIR_ALIGNAS(n) alignas(n)
#elif defined(_MSC_VER)
  #define SAFIR_ALIGNAS(n) __declspec(align(n))
#elif defined(__GNUC__) || defined(__clang__)
  #define SAFIR_ALIGNAS(n) __attribute__((aligned(n)))
#else
  #define SAFIR_ALIGNAS(n) _Alignas(n)
#endif

#define SAFIR_ALIGNMENT 16ULL
#define SAFIR_ALIGN_UP(val, align) (((val) + ((align) - 1ULL)) & ~((align) - 1ULL))

typedef struct SafirPoolArena {
    const char *name;
    uint8_t *buffer;
    size_t capacity;
    size_t used;
    size_t high_watermark;
    size_t alloc_count;
} SafirPoolArena;

/* Static arenas backing the pools - strictly zero dynamic allocations */
#ifdef SAFIR_OCR_WEIGHTS_EXTERNAL
/* Switch: the model's memory comes from safir_pool_attach (sized to the chosen model). */
SAFIR_ALIGNAS(16) static uint8_t s_arena_ocr_weights[16];
#define SAFIR_STATIC_OCR_WEIGHTS_BYTES 0ULL
#else
SAFIR_ALIGNAS(16) static uint8_t s_arena_ocr_weights[SAFIR_POOL_OCR_WEIGHTS_MAX_BYTES];
#define SAFIR_STATIC_OCR_WEIGHTS_BYTES SAFIR_POOL_OCR_WEIGHTS_MAX_BYTES
#endif
SAFIR_ALIGNAS(16) static uint8_t s_arena_ocr_scratch[SAFIR_POOL_OCR_SCRATCH_MAX_BYTES];
SAFIR_ALIGNAS(16) static uint8_t s_arena_dict_index[SAFIR_POOL_DICT_INDEX_MAX_BYTES];
SAFIR_ALIGNAS(16) static uint8_t s_arena_ui_state[SAFIR_POOL_UI_STATE_MAX_BYTES];

static SafirPoolArena s_arenas[POOL_COUNT];
static bool s_initialized = false;
/* Memory handed in with safir_pool_attach (NULL: the static arena). */
static uint8_t *s_attached_buf[POOL_COUNT];
static size_t s_attached_cap[POOL_COUNT];

static void ensure_arenas_configured(void) {
    s_arenas[POOL_OCR_WEIGHTS].name = "POOL_OCR_WEIGHTS";
    s_arenas[POOL_OCR_WEIGHTS].buffer = s_arena_ocr_weights;
    s_arenas[POOL_OCR_WEIGHTS].capacity = SAFIR_STATIC_OCR_WEIGHTS_BYTES;

    s_arenas[POOL_OCR_SCRATCH].name = "POOL_OCR_SCRATCH";
    s_arenas[POOL_OCR_SCRATCH].buffer = s_arena_ocr_scratch;
    s_arenas[POOL_OCR_SCRATCH].capacity = SAFIR_POOL_OCR_SCRATCH_MAX_BYTES;

    s_arenas[POOL_DICT_INDEX].name = "POOL_DICT_INDEX";
    s_arenas[POOL_DICT_INDEX].buffer = s_arena_dict_index;
    s_arenas[POOL_DICT_INDEX].capacity = SAFIR_POOL_DICT_INDEX_MAX_BYTES;

    s_arenas[POOL_UI_STATE].name = "POOL_UI_STATE";
    s_arenas[POOL_UI_STATE].buffer = s_arena_ui_state;
    s_arenas[POOL_UI_STATE].capacity = SAFIR_POOL_UI_STATE_MAX_BYTES;

    for (size_t i = 0; i < (size_t)POOL_COUNT; ++i) {
        if (s_attached_buf[i]) {
            s_arenas[i].buffer = s_attached_buf[i];
            s_arenas[i].capacity = s_attached_cap[i];
        }
    }
}

bool safir_pool_attach(SafirPool pool, void *buf, size_t cap) {
    if ((int)pool < 0 || pool >= POOL_COUNT) return false;
    if (buf && ((uintptr_t)buf & (SAFIR_ALIGNMENT - 1ULL)) != 0) return false;
    if (!s_initialized) safir_ledger_init();
    s_attached_buf[pool] = buf ? (uint8_t *)buf : NULL;
    s_attached_cap[pool] = buf ? cap : 0;
    ensure_arenas_configured();
    s_arenas[pool].used = 0;
    s_arenas[pool].high_watermark = 0;
    s_arenas[pool].alloc_count = 0;
    return true;
}

void safir_ledger_init(void) {
    ensure_arenas_configured();
    for (size_t i = 0; i < (size_t)POOL_COUNT; ++i) {
        s_arenas[i].used = 0;
        s_arenas[i].high_watermark = 0;
        s_arenas[i].alloc_count = 0;
    }
    s_initialized = true;
}

void *safir_alloc(SafirPool pool, size_t bytes) {
    if ((int)pool < 0 || pool >= POOL_COUNT || bytes == 0) {
        return NULL;
    }

    if (!s_initialized) {
        safir_ledger_init();
    }

    SafirPoolArena *arena = &s_arenas[pool];
    size_t aligned_offset = SAFIR_ALIGN_UP(arena->used, SAFIR_ALIGNMENT);

    if (aligned_offset > arena->capacity || bytes > arena->capacity - aligned_offset) {
        /* Pool exhausted: strictly fail instead of silently overflowing */
        return NULL;
    }

    void *ptr = &arena->buffer[aligned_offset];
    arena->used = aligned_offset + bytes;
    if (arena->used > arena->high_watermark) {
        arena->high_watermark = arena->used;
    }
    arena->alloc_count++;

    return ptr;
}

void safir_pool_reset(SafirPool pool) {
    if ((int)pool < 0 || pool >= POOL_COUNT) {
        return;
    }
    if (!s_initialized) {
        safir_ledger_init();
    }
    s_arenas[pool].used = 0;
}

void safir_pool_rewind(SafirPool pool, size_t mark) {
    if ((int)pool < 0 || pool >= POOL_COUNT) {
        return;
    }
    if (!s_initialized) {
        safir_ledger_init();
    }
    if (mark < s_arenas[pool].used) {
        s_arenas[pool].used = mark;
    }
}

bool safir_ledger_get_stats(SafirPool pool, SafirPoolStats *out_stats) {
    if ((int)pool < 0 || pool >= POOL_COUNT || !out_stats) {
        return false;
    }
    if (!s_initialized) {
        safir_ledger_init();
    }
    const SafirPoolArena *arena = &s_arenas[pool];
    out_stats->name = arena->name;
    out_stats->capacity = arena->capacity;
    out_stats->used = arena->used;
    out_stats->high_watermark = arena->high_watermark;
    out_stats->alloc_count = arena->alloc_count;
    return true;
}

size_t safir_pool_used(SafirPool pool) {
    if ((int)pool < 0 || pool >= POOL_COUNT) {
        return 0;
    }
    if (!s_initialized) {
        safir_ledger_init();
    }
    return s_arenas[pool].used;
}

size_t safir_pool_capacity(SafirPool pool) {
    if ((int)pool < 0 || pool >= POOL_COUNT) {
        return 0;
    }
    if (!s_initialized) {
        safir_ledger_init();
    }
    return s_arenas[pool].capacity;
}

size_t safir_ledger_total_used(void) {
    if (!s_initialized) {
        safir_ledger_init();
    }
    size_t total = 0;
    for (size_t i = 0; i < (size_t)POOL_COUNT; ++i) {
        total += s_arenas[i].used;
    }
    return total;
}

size_t safir_ledger_total_capacity(void) {
    if (!s_initialized) {
        safir_ledger_init();
    }
    size_t total = 0;
    for (size_t i = 0; i < (size_t)POOL_COUNT; ++i) {
        total += s_arenas[i].capacity;
    }
    return total;
}

void safir_ledger_dump_stats(void) {
    if (!s_initialized) {
        safir_ledger_init();
    }

    printf("======================================================================\n");
    printf(" safir Memory Ledger Status (Total Budget: %zu bytes)\n", (size_t)SAFIR_TOTAL_BUDGET_MAX_BYTES);
    printf("----------------------------------------------------------------------\n");
    printf(" %-20s | %10s | %10s | %10s | %5s\n", "Pool Name", "Used", "High-Water", "Capacity", "Alloc");
    printf("----------------------------------------------------------------------\n");

    for (size_t i = 0; i < (size_t)POOL_COUNT; ++i) {
        const SafirPoolArena *a = &s_arenas[i];
        printf(" %-20s | %10zu | %10zu | %10zu | %5zu\n",
               a->name, a->used, a->high_watermark, a->capacity, a->alloc_count);
    }
    printf("----------------------------------------------------------------------\n");
    printf(" Total Static Used: %zu / %zu bytes (%.2f%% utilized)\n",
           safir_ledger_total_used(),
           safir_ledger_total_capacity(),
           (double)safir_ledger_total_used() * 100.0 / (double)safir_ledger_total_capacity());
    printf("======================================================================\n");
}
