#ifndef SAFIR_DICT_H
#define SAFIR_DICT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "safir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAFIR_DICT_MAGIC 0x44504E56 /* "VNPD" */
#define SAFIR_DICT_VERSION 2
#define SAFIR_DICT_BLOCK_SIZE 16384 /* 16 KB uncompressed glossary block */

#define SAFIR_L1_PAGE_ENTRIES 64    /* Every 64th entry forms a Level-1 anchor */
#define SAFIR_L2_PAGE_SIZE    32768 /* Max Level-2 page the reader accepts (32 KB buffer in POOL_UI_STATE).
                                     * Was 4096, which rejected every page of the real JMdict pack (10-29 KB). */

/* Compression types */
#define SAFIR_COMPRESS_NONE    0
#define SAFIR_COMPRESS_DEFLATE 1

/* Header flags */
#define SAFIR_DICT_FLAG_RANKED 0x0001 /* ranked pack (score + priority tags); preferred by the loader */

/* Part of Speech bitmasks */
#define SAFIR_POS_OTHER        (1U << 0)
#define SAFIR_POS_NOUN         (1U << 1)
#define SAFIR_POS_VERB_ICHIDAN (1U << 2) /* v1 / 一段 */
#define SAFIR_POS_VERB_GODAN   (1U << 3) /* v5 / 五段 */
#define SAFIR_POS_VERB_SURU    (1U << 4) /* vs / サ変 */
#define SAFIR_POS_VERB_KURU    (1U << 5) /* vk / カ変 */
#define SAFIR_POS_ADJ_I        (1U << 6) /* adj-i / 形容詞 */
#define SAFIR_POS_ADJ_NA       (1U << 7) /* adj-na / 形容動詞 */
#define SAFIR_POS_ADVERB       (1U << 8) /* adv / 副詞 */
#define SAFIR_POS_PREFIX       (1U << 9)
#define SAFIR_POS_SUFFIX       (1U << 10)
#define SAFIR_POS_PARTICLE     (1U << 11)

#define SAFIR_POS_VERB_ANY     (SAFIR_POS_VERB_ICHIDAN | SAFIR_POS_VERB_GODAN | \
                                SAFIR_POS_VERB_SURU | SAFIR_POS_VERB_KURU)
#define SAFIR_POS_ALL          0xFFFFU

#pragma pack(push, 1)
/* 64-byte Header of safir_dict.bin (Version 2) */
typedef struct SafirDictHeader {
    uint32_t magic;               /* "VNPD" */
    uint32_t version;             /* Version 2 */
    uint32_t header_size;         /* 64 bytes */
    uint16_t compression_type;    /* SAFIR_COMPRESS_NONE or SAFIR_COMPRESS_DEFLATE */
    uint16_t flags;               /* Reserved flags */
    uint32_t entry_count;         /* Total number of sorted lookup entries */
    uint32_t block_count;         /* Number of glossary blocks */
    uint32_t block_size;          /* Uncompressed block size (16384) */
    uint32_t l1_anchor_count;     /* Number of Level-1 sparse page anchors */
    uint32_t l1_anchors_offset;   /* File offset where Level-1 anchors start */
    uint32_t l1_anchors_bytes;    /* Size in bytes of Level-1 anchors */
    uint32_t l2_pages_offset;     /* File offset where Level-2 pages start */
    uint32_t l2_pages_bytes;      /* Total size in bytes of all Level-2 pages */
    uint32_t blocks_offset;       /* File offset where block metadata & chunks start */
    uint32_t blocks_bytes;        /* Total size of blocks section */
    uint32_t reserved[2];         /* 64 bytes total header */
} SafirDictHeader;

/* 56-byte Level-1 Sparse Index Anchor stored in POOL_DICT_INDEX */
typedef struct SafirL1Anchor {
    char headword[44];            /* Null-terminated UTF-8 term of first entry in page */
    uint32_t file_offset;         /* File offset where Level-2 page begins */
    uint16_t page_bytes;          /* Size in bytes of this Level-2 page (<= 4096) */
    uint16_t entry_count;         /* Number of entries in this Level-2 page (<= 64) */
    uint32_t first_entry_idx;     /* Global index of the first entry in this page */
} SafirL1Anchor;

/* Level-2 Page Header (8 bytes at start of each Level-2 page on disk) */
typedef struct SafirL2PageHeader {
    uint16_t entry_count;         /* Number of entries in this page */
    uint16_t string_bytes;        /* Length of packed string table */
    uint32_t reserved;
} SafirL2PageHeader;

/* 16-byte Compact Disk / Memory Index Entry */
typedef struct SafirDiskIndexEntry {
    uint32_t term_offset;         /* Byte offset into Level-2 page's local string_table */
    uint16_t block_id;            /* ID of glossary chunk block */
    uint16_t offset_in_block;     /* Offset of word record in uncompressed 16KB block */
    uint16_t gloss_len;           /* Length in bytes of glossary string */
    uint16_t pos_mask;            /* Bitmask of Part of Speech */
    uint16_t pitch_mask;          /* Bitmask of pitch positions */
    uint16_t freq_rank;           /* Frequency ranking (1 = most common, 0 = unranked) */
} SafirDiskIndexEntry;

/* 8-byte Block Metadata */
typedef struct SafirBlockMeta {
    uint32_t file_offset;         /* Offset in file where compressed block payload starts */
    uint16_t comp_size;           /* Size of compressed payload in bytes */
    uint16_t uncomp_size;         /* Uncompressed size in bytes (<= 16384) */
} SafirBlockMeta;
#pragma pack(pop)

/* Lookup Result */
typedef struct SafirLookupResult {
    char headword[64];            /* Headword / Kanji (null-terminated UTF-8) */
    char reading[64];             /* Kana reading (null-terminated UTF-8) */
    char glossary[512];           /* Definition text (null-terminated UTF-8) */
    char deinflect_rule[48];      /* Deinflection chain, base first (e.g. "causative+passive+past") */
    uint16_t matched_chars;       /* Number of characters matched from start_char_idx */
    uint16_t matched_bytes;       /* Length of surface substring in bytes */
    uint16_t pos_mask;            /* Matched POS */
    uint16_t pitch_mask;          /* Pitch accent bitmask */
    uint16_t freq_rank;           /* Frequency rank */
    bool found;                   /* True if a matching dictionary term was found */
} SafirLookupResult;

/* One dictionary entry that matches text at a position, for the popup's entry list.
 * Small on purpose: headword, reading and glossary are read with safir_dict_read_entry()
 * only for the entries actually shown. */
typedef struct SafirDictMatch {
    uint16_t block_id;
    uint16_t offset_in_block;
    uint16_t gloss_len;
    uint16_t pos_mask;
    uint16_t freq_rank;           /* 1 = most common; reading-only keys +32768; 0 = unranked */
    uint16_t matched_chars;       /* characters of the line this entry covers */
    uint16_t matched_bytes;
    uint8_t steps;                /* deinflection rules applied (0 = exact) */
    char rule[48];                /* e.g. "-te+-te iru+past", empty when exact */
} SafirDictMatch;

/* Word Token Boundary for Sentence Navigation (D-Pad Left / Right) */
#define SAFIR_MAX_TOKENS_PER_LINE 64

typedef struct SafirTokenSpan {
    uint16_t start_char_idx;
    uint16_t char_count;
    bool is_dict_word;
} SafirTokenSpan;

typedef struct SafirLineTokens {
    SafirTokenSpan tokens[SAFIR_MAX_TOKENS_PER_LINE];
    uint16_t token_count;
} SafirLineTokens;

/* SafirDict instance with Two-Level Disk-Backed Paging */
typedef struct SafirDict {
    SafirDictHeader header;
    const SafirL1Anchor* l1_anchors;      /* Pointer to Level-1 anchors in POOL_DICT_INDEX */
    uint32_t l1_anchor_count;             /* Count of Level-1 anchors */
    const SafirBlockMeta* block_metas;    /* Pointer to block table in POOL_DICT_INDEX */
    FILE* file_handle;                    /* Open file handle if backed by disk */
    const uint8_t* raw_file_memory;       /* Pointer if backed by memory */
    size_t raw_file_size;

    /* Single Level-2 page cache (SAFIR_L2_PAGE_SIZE) in POOL_UI_STATE */
    uint8_t* cached_l2_buffer;            /* SAFIR_L2_PAGE_SIZE bytes in POOL_UI_STATE */
    int32_t cached_l2_anchor_id;          /* Currently cached anchor ID (-1 if none) */

    /* Single 16 KB uncompressed glossary cache in POOL_UI_STATE */
    uint8_t* cached_block_buffer;         /* 16384 bytes in POOL_UI_STATE */
    int32_t cached_block_id;              /* Currently cached block ID (-1 if none) */

    /* Optional LRU of extra L2 pages (safir_dict_set_page_cache) */
#define SAFIR_DICT_MAX_CACHE_PAGES 8
    uint8_t* page_cache;
    uint8_t page_cache_slots;
    int32_t page_cache_anchor[SAFIR_DICT_MAX_CACHE_PAGES];
    uint32_t page_cache_age[SAFIR_DICT_MAX_CACHE_PAGES];
    uint32_t page_cache_clock;
    uint32_t page_reads;
    const uint8_t* cur_page;               /* page the last ensure_l2_page_cached() returned */

    /* 256-entry 1-byte radix prefix jump table on Level-1 anchors */
    uint32_t prefix_table[256];
} SafirDict;

bool safir_dict_load_file(SafirDict* dict, const char* filepath);
/* Human-readable reason for the last safir_dict_load_file() result (static buffer). */
const char* safir_dict_last_error(void);
bool safir_dict_load_memory(SafirDict* dict, const void* data, size_t size);
void safir_dict_close(SafirDict* dict);

/* Exact lookup of a null-terminated UTF-8 term (with optional pos_filter) */
bool safir_dict_lookup_exact(SafirDict* dict, const char* term, uint16_t pos_filter, SafirLookupResult* out);

/* Longest-match scanner at start_char_idx in text line with deinflection */
bool safir_dict_scan_longest(SafirDict* dict, const SafirTextLine* line, uint16_t start_char_idx, SafirLookupResult* out);

/* Every entry matching text at start_char_idx, Yomitan order: longer source text first,
 * then fewer deinflection steps, then more common. Returns how many were written. */
size_t safir_dict_lookup_all(SafirDict* dict, const SafirTextLine* line, uint16_t start_char_idx,
                             SafirDictMatch* out, size_t max_out);

/* Reads one entry's strings. Any output pointer may be NULL. Never cuts a UTF-8 character. */
bool safir_dict_read_entry(SafirDict* dict, const SafirDictMatch* m, char* headword, size_t headword_cap,
                           char* reading, size_t reading_cap, char* glossary, size_t glossary_cap);

/* Optional extra Level-2 page cache (whole pages, LRU), so a lookup that tries many
 * deinflection candidates does not re-read the same 10-30 KB pages from SD. bytes is
 * split into SAFIR_L2_PAGE_SIZE slots; NULL/0 turns it off. The buffer stays the caller's. */
void safir_dict_set_page_cache(SafirDict* dict, void* buf, size_t bytes);

/* Number of Level-2 page reads from the file since load (diagnostics). */
uint32_t safir_dict_page_reads(const SafirDict* dict);

/* Sentence tokenization for D-Pad Left/Right navigation */
void safir_dict_tokenize_line(SafirDict* dict, const SafirTextLine* line, SafirLineTokens* out_tokens);
int safir_tokens_next(const SafirLineTokens* tokens, uint16_t cur_char_idx);
int safir_tokens_prev(const SafirLineTokens* tokens, uint16_t cur_char_idx);

#ifdef __cplusplus
}
#endif

#endif /* SAFIR_DICT_H */
