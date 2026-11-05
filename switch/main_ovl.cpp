#if defined(__SWITCH__)
#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <switch.h>

/*
 * NOTE (audit 2026-11-04): nx-ovlloader hands the overlay its heap through the
 * homebrew ABI heap override, and libnx uses that override instead of
 * __nx_heap_size. The value below is therefore ignored under nx-ovlloader; it is
 * kept only so a stand-alone launch does not grab all remaining memory.
 */
extern "C" {
    size_t __nx_heap_size = 0x140000;
}

#include "mock_libnx.h"
#include "overlay_ui.h"
#include "bg_worker.h"
#include "safir_ledger.h"
#include "safir_crop.h"
#include "games_ini.h"
#include "safir_text.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdarg>
#include <cctype>
#include <algorithm>
#include <climits>
#include <sys/stat.h>
#include <string>
#include <vector>

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static SafirDict s_switch_dict;
static SafirOverlayConfig s_switch_cfg;
static SafirOcrBackend *s_switch_ocr = nullptr;
static bool s_dict_loaded = false;
static bool s_ocr_loaded = false;
static bool s_assets_probed = false;

static Result s_rc_capssc = 0;
/* What a scan captures: the full screen, or the games.ini region for this title. Owned
 * here (set at the END of initServices) because safir_bg_worker_init() and safir_ui_init()
 * both reset their own copy to the old 90,480 1100x210 default. */
static SafirRect s_scan_rect = { 0, 0, 1280, 720 };
static Result s_rc_pminfo = 0;
/* Heap a scan needs: only the 640x360 grey crop (225 KB), allocated while the scan runs.
 * The caps:sc JPEG (up to ~500 KB) is written into the OCR model's own memory, which the
 * model lends out and re-reads from the SD card afterwards (safir_ocr_vocr_lend_weights).
 * Keeping a 742 KB screenshot block on the heap is what starved the popup's fonts under
 * the default 4 MB nx-ovlloader heap. */
static uint8_t *s_capture_block = nullptr;
static const size_t kFrameCropBytes = 640u * 360u;
static const size_t kCaptureBlockBytes = kFrameCropBytes;
static u32 s_ocr_load_ms = 0;   /* model read + self-test, for the status line */
static u32 s_last_scan_ms = 0;  /* capture + OCR + lookup of the last scan */
static u32 s_reload_ms = 0;     /* re-reading the OCR model after the capture borrowed its memory */

static char s_dict_status[160] = "Dict: not loaded yet";
static char s_region_status[160] = "";
static char s_ocr_status[160]  = "OCR: not loaded yet";
static char s_pill[192] = "";      /* status pill at the top of the screen */
static char s_detail[768] = "";    /* explanation box shown when a scan finds nothing */

/* Words of the last scan, segmented left to right by longest dictionary match (with
 * deinflection), with their screen boxes for touch and popup placement. Heap (std::vector)
 * is fine here: main_ovl.cpp is the heap owner. The full entry list of a word is looked up
 * only when it is selected (load_entries). */
struct ScanWord {
    std::string surface;   /* as it appears on screen, e.g. 食べさせられなかった */
    std::string headword;  /* best entry's dictionary form, e.g. 食べる */
    unsigned line;
    uint16_t char_start, char_count;
    s32 x0, y0, x1, y1;    /* union of the word's character boxes, screen pixels (1280x720) */
};
static std::vector<ScanWord> s_scan_words;
/* The recognized lines (codepoints), for the context strip and for lookups on selection. */
static std::vector<std::vector<uint32_t>> s_scan_lines;
/* Screen box of every recognized line (x0, y0, x1, y1), so the popup can avoid the text. */
static s32 s_line_box[SAFIR_MAX_LINES_PER_BOX][4];
static unsigned s_line_box_n = 0;
static const size_t kMaxScanWords = 96;

/* Frames left to draw a fully transparent layer before capturing. libtesla adds its
 * layer to the Screenshot layer stack, so a capture taken while the panel is drawn
 * contains the panel itself over the left 448 px of the game. */
static int s_capture_countdown = 0;
static const int kBlankFramesBeforeCapture = 4;
/* First scan after the overlay opens waits longer: the game is still being moved out of
 * focus, and capturing during that transition is suspect for the failures seen right after
 * opening. Failed captures are retried automatically, with a growing delay. */
static const int kFramesBeforeFirstCapture = 20;
static int s_capture_failures = 0;
static const int kMaxAutoRetries = 2;

/* Result of the current scan (~16 KB, off the stack). */
static SafirOcrResult s_ocr_res;

/* Search order: both data folders, every accepted file name. */
static const char *s_asset_dirs[] = { "sdmc:/switch/safir/", "sdmc:/safir/" };
static const char *s_dict_names[] = { "dict.bin", "safir_dict.bin", "jmdict.bin" };
static const char *s_ocr_names[]  = { "VOCR6.2_S_w8.vocr", "ocr.vocr", "ocr", "model.vocr" };
/* The Normal model: about half the Small model's character errors, 1.19 MB instead of 0.82 MB. */
static const char *s_ocr_normal_names[] = { "VOCR6.2_N_w8g2.vocr", "VOCR6.2_N_w8.vocr" };
/* config.ini ocr_model: auto (Normal when the heap has room for it, else Small), small, normal. */
enum { OCR_MODEL_AUTO = 0, OCR_MODEL_SMALL = 1, OCR_MODEL_NORMAL = 2 };
static int s_ocr_model_pref = OCR_MODEL_AUTO;
/* config.ini save_scans=1: keep each scan's screenshot (scan_1..4.jpg in sdmc:/switch/safir/). */
static bool s_save_scans = false;
/* Chosen at start-up (choose_ocr_model): the file, and the heap block POOL_OCR_WEIGHTS lives in,
 * sized to that model. The Switch build has no static weights pool (SAFIR_OCR_WEIGHTS_EXTERNAL). */
static char s_ocr_model_path[96] = "";
static const char *s_ocr_model_label = "none";
static char s_ocr_model_note[192] = "";
/* Backend and context next to the blob in the pool: 20.6 KB measured on the host. */
static const size_t kOcrContextBytes = 32 * 1024;

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Copy UTF-8 into dst without ever cutting a multi-byte character in half. */
static void copy_utf8_trunc(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && (((unsigned char)src[n]) & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Writes what the OCR actually saw, so a failed scan can be reproduced on a PC:
 *   sdmc:/switch/safir/debug_screen.jpg  the full caps:sc screenshot
 *   sdmc:/switch/safir/debug_crop.pgm    the 2x grey crop handed to the recognizer */
static void dump_debug_capture(const uint8_t *crop, uint16_t w, uint16_t h) {
    FILE *f = fopen("sdmc:/switch/safir/debug_crop.pgm", "wb");
    if (f) {
        fprintf(f, "P5\n%u %u\n255\n", (unsigned)w, (unsigned)h);
        fwrite(crop, 1, (size_t)w * h, f);
        fclose(f);
    }
    /* No debug_screen.jpg any more: the JPEG lives in the OCR model's memory during a
     * scan and is overwritten when the model is re-read. */
}

/* What the two-pass scan looked at, for bug reports: sdmc:/switch/safir/debug_scan.txt. */
static void write_scan_log(const SafirScanPlan *plan, unsigned lines_read) {
    FILE *f = fopen("sdmc:/switch/safir/debug_scan.txt", "w");
    if (!f) return;
    fprintf(f, "pass 1: %u candidate lines; pass 2: %u bands; %u Japanese lines read\n",
            (unsigned)plan->pass1_lines, (unsigned)plan->count, lines_read);
    for (unsigned j = 0; j < plan->count; ++j) {
        const SafirScanBlock *b = &plan->blocks[j];
        fprintf(f, "band %u: %ux%u at %u,%u, line %u px, scale %u, %s text\n", j, b->screen.w, b->screen.h,
                b->screen.x, b->screen.y, b->line_h, b->scale, b->dark ? "dark" : "light");
    }
    fprintf(f, "%s\n", safir_ocr_vocr_last_error());
    fclose(f);
}

/* Event log for bug reports: sdmc:/switch/safir/events.log, rewritten each time the overlay
 * loads (at most kMaxEvents lines). Records opens, closes, scan requests and what each scan
 * read, to find why reopening with the scan combo could show the previous scan (2026-11-05).
 * Main thread only; onHide() (which libtesla may call from its input thread) just counts. */
static unsigned s_event_lines = 0;
static const unsigned kMaxEvents = 400;
static u64 s_event_t0 = 0;
static u32 s_scan_no = 0;
static volatile u32 s_hide_count = 0;
static u32 s_hide_logged = 0;
static void evlog(const char *fmt, ...) {
    if (s_event_lines >= kMaxEvents) return;
    if (s_event_t0 == 0) s_event_t0 = armGetSystemTick();
    FILE *f = fopen("sdmc:/switch/safir/events.log", s_event_lines == 0 ? "w" : "a");
    if (!f) return;
    fprintf(f, "%8llu ms  ", (unsigned long long)(armTicksToNs(armGetSystemTick() - s_event_t0) / 1000000ULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
    s_event_lines++;
}

/* Reads only the 64-byte header. Returns entry count, or -1 with *why set. */
static long probe_dict_header(const char *path, const char **why, uint16_t *flags = nullptr) {
    FILE *f = fopen(path, "rb");
    if (!f) { *why = "cannot open"; return -1; }
    SafirDictHeader hdr;
    size_t rd = fread(&hdr, sizeof(hdr), 1, f);
    fclose(f);
    if (rd != 1) { *why = "shorter than header"; return -1; }
    if (hdr.magic != SAFIR_DICT_MAGIC) { *why = "not a VNPD file"; return -1; }
    if (hdr.version != SAFIR_DICT_VERSION) { *why = "old VNPD version, repack"; return -1; }
    *why = nullptr;
    if (flags) *flags = hdr.flags;
    return (long)hdr.entry_count;
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* Which dictionary to use. A ranked pack (SAFIR_DICT_FLAG_RANKED, written by the current
 * tools/dict_packer.py) always wins over an older file, then the search order
 * (sdmc:/switch/safir/dict.bin first). The old rule, "most entries wins", picked a stale
 * unranked 896k-entry pack over the new 554k-entry one: は -> 破, が -> 駕. Files under
 * 1000 entries (test fixtures) are used only when nothing else is there. */
static void *s_page_cache = nullptr; /* allocated in initServices, before the framebuffers */
static const size_t kPageCacheBytes = 2 * SAFIR_L2_PAGE_SIZE; /* 64 KB: ~8x fewer SD reads per scan */

static void load_dictionary(void) {
    char best_path[96] = "";
    long best_score = -1;
    long best_entries = 0;
    uint16_t best_flags = 0;
    int valid = 0;
    char first_bad[160] = "";

    int order = 0;
    for (size_t d = 0; d < ARRAY_LEN(s_asset_dirs); ++d) {
        for (size_t n = 0; n < ARRAY_LEN(s_dict_names); ++n, ++order) {
            char path[96];
            snprintf(path, sizeof(path), "%s%s", s_asset_dirs[d], s_dict_names[n]);
            if (!file_exists(path)) continue;
            const char *why = nullptr;
            uint16_t flags = 0;
            long entries = probe_dict_header(path, &why, &flags);
            if (entries < 0) {
                if (!first_bad[0]) snprintf(first_bad, sizeof(first_bad), "%s: %s", path, why);
                continue;
            }
            valid++;
            long score = (entries >= 1000 ? 2000 : 0) + ((flags & SAFIR_DICT_FLAG_RANKED) ? 1000 : 0) + (100 - order);
            if (score > best_score) {
                best_score = score;
                best_entries = entries;
                best_flags = flags;
                snprintf(best_path, sizeof(best_path), "%s", path);
            }
        }
    }

    if (best_score < 0) {
        if (first_bad[0]) {
            snprintf(s_dict_status, sizeof(s_dict_status), "Dict ERR %s", first_bad);
        } else {
            snprintf(s_dict_status, sizeof(s_dict_status),
                     "Dict ERR: no dict.bin in sdmc:/switch/safir/ or sdmc:/safir/");
        }
        return;
    }

    /* POOL_DICT_INDEX and POOL_UI_STATE belong to the dictionary alone; start clean. */
    safir_pool_reset(POOL_DICT_INDEX);
    safir_pool_reset(POOL_UI_STATE);
    if (!safir_dict_load_file(&s_switch_dict, best_path)) {
        snprintf(s_dict_status, sizeof(s_dict_status), "Dict ERR %s: %s", best_path, safir_dict_last_error());
        return;
    }
    s_dict_loaded = true;
    safir_dict_set_page_cache(&s_switch_dict, s_page_cache, s_page_cache ? kPageCacheBytes : 0);

    const char *extra = valid > 1 ? ", other dict files ignored" : "";
    if (best_entries < 1000) {
        snprintf(s_dict_status, sizeof(s_dict_status),
                 "Dict WARN: %s has only %ld entries (test fixture?)", best_path, best_entries);
    } else if (!(best_flags & SAFIR_DICT_FLAG_RANKED)) {
        snprintf(s_dict_status, sizeof(s_dict_status),
                 "Dict WARN: %s is an old unranked pack (%ld entries): copy the new dict.bin%s", best_path,
                 best_entries, extra);
    } else {
        snprintf(s_dict_status, sizeof(s_dict_status), "Dict OK: %s (%ld entries%s)", best_path, best_entries, extra);
    }
}

static void load_ocr_model(void) {
    const char *path = s_ocr_model_path;
    if (!path[0]) {
        snprintf(s_ocr_status, sizeof(s_ocr_status), "OCR ERR: %s",
                 s_ocr_model_note[0] ? s_ocr_model_note : "no OCR model in sdmc:/switch/safir/ or sdmc:/safir/");
        return;
    }

    /* POOL_OCR_WEIGHTS belongs to the local OCR backend alone; a failed earlier
     * attempt may have left partial allocations behind, so rebuild from scratch. */
    safir_pool_reset(POOL_OCR_WEIGHTS);
    safir_pool_reset(POOL_OCR_SCRATCH); /* the load-time self-test borrows scratch */
    /* No dHash cache: it fingerprints the scanned area at 9x8, and for a full-screen scan a
     * new dialogue line barely changes that, so every rescan returned the first result. */
    SafirOcrConfig ocr_cfg = {SAFIR_DECODER_GREEDY, 1, 0.5f, false};
    s_switch_ocr = safir_ocr_vocr_create(&ocr_cfg);
    if (!s_switch_ocr) {
        snprintf(s_ocr_status, sizeof(s_ocr_status), "OCR ERR: POOL_OCR_WEIGHTS OOM creating backend (cap %zu B)",
                 safir_pool_capacity(POOL_OCR_WEIGHTS));
        return;
    }
    u64 t0 = armGetSystemTick();
    bool model_ok = safir_ocr_vocr_load_file(s_switch_ocr, path);
    s_ocr_load_ms = (u32)(armTicksToNs(armGetSystemTick() - t0) / 1000000ULL);
    if (!model_ok) {
        snprintf(s_ocr_status, sizeof(s_ocr_status), "OCR ERR %s: %s", path, safir_ocr_vocr_last_error());
        return;
    }
    s_ocr_loaded = true;
    safir_bg_worker_set_backend(s_switch_ocr);
    if (safir_ocr_vocr_is_placeholder()) {
        snprintf(s_ocr_status, sizeof(s_ocr_status), "OCR WARN: %s is a synthetic test model (PLACEHOLDER)", path);
    } else if (safir_ocr_vocr_selftest_result() != 1) {
        /* The network ran but misread the built-in test line: likely a miscompiled
         * NEON path. Rebuild with -DVOCR_NO_NEON (see AUDIT) and report it. */
        snprintf(s_ocr_status, sizeof(s_ocr_status), "OCR WARN: self-test FAILED, results unreliable");
    } else {
        snprintf(s_ocr_status, sizeof(s_ocr_status), "OCR OK: VOCR6.2 %s, self-test pass, %u ms", s_ocr_model_label,
                 (unsigned)s_ocr_load_ms);
    }
}

/* Allocated on the first frame, after libtesla has its framebuffers, so a tight
 * heap fails here (reported in the scan result) instead of crashing at startup. */
/* Largest block malloc can give right now (64 KB steps), for out-of-memory reports. */
/* Called through volatile pointers: GCC deletes a malloc() immediately followed by
 * free() and assumes it succeeded, which made this report "4096 KB free" on every console
 * and the popup always pick its largest size. */
static void *(*volatile s_probe_malloc)(size_t) = malloc;
static void (*volatile s_probe_free)(void *) = free;

static size_t largest_free_block(void) {
    size_t lo = 0, hi = 16u << 20; /* binary search, 16 KB resolution */
    while (hi - lo > 0x4000) {
        size_t mid = lo + (hi - lo) / 2;
        void *p = s_probe_malloc(mid);
        if (p) { s_probe_free(p); lo = mid; }
        else hi = mid;
    }
    return lo;
}

static char s_heap_note[160] = "";
static char s_res_note[96] = "";

/* Two RGBA4444 framebuffers, heights rounded up to 128 rows as libnx does. */
static size_t framebuffer_cost(unsigned w, unsigned h) {
    size_t row = ((size_t)w * 2 + 63) & ~(size_t)63;
    size_t rows = ((size_t)h + 127) & ~(size_t)127;
    return 2 * row * rows;
}

/* Runs in initServices, BEFORE libtesla allocates its framebuffers.
 *
 * The overlay no longer draws a whole-screen framebuffer that vi scales up (that was the
 * blur: 768x432 stretched to 1280x720). It draws a popup-sized layer at exactly one
 * framebuffer pixel per screen pixel and moves it next to the selected word. The popup
 * itself is small; a whole-screen layer at native resolution would need 3.9 MB, which is
 * what made the old approach trade sharpness for memory. 640x384: 0.98 MB, 768x384: 1.18 MB. */
static int s_forced_popup_width = 0; /* popup_width from config.ini; 0 = automatic */

static char s_mem_head[256] = "";
static size_t s_free_at_start = 0; /* largest free block before the framebuffers (KB in the notes) */

static void choose_popup_size(void) {
    /* At the peak of a scan the heap holds the framebuffers, the 742 KB capture block and
     * the C++ state; between scans, the framebuffers and the glyph cache (256 KB budget).
     * The first size whose peak still leaves kReserve free wins. With the default 4 MB
     * nx-ovlloader heap about 2 MB is left for the overlay: 640x384. */
    static const unsigned sizes[][2] = { { 768, 384 }, { 704, 384 }, { 640, 384 }, { 576, 384 }, { 576, 256 }, { 512, 256 } };
    /* nv transfer memory for the framebuffers (256 KB, allocated after this runs) plus
     * libtesla's and our own small allocations. */
    const size_t kReserve = 416 * 1024;
    s_free_at_start = largest_free_block();
    unsigned pw = 512, ph = 256;
    if (s_forced_popup_width) {
        pw = (unsigned)s_forced_popup_width;
        ph = 384;
    } else {
        for (const auto &sz : sizes) {
            if (framebuffer_cost(sz[0], sz[1]) + kCaptureBlockBytes + kReserve <= s_free_at_start) {
                pw = sz[0];
                ph = sz[1];
                break;
            }
        }
    }
    tsl::cfg::FloatingLayer = true;
    tsl::cfg::FullScreenFbWidth = (u16)pw;
    tsl::cfg::FullScreenFbHeight = (u16)ph;
    snprintf(s_res_note, sizeof(s_res_note), "Popup %ux%u (%s), heap %zu KB free at start", pw, ph,
             s_forced_popup_width ? "config" : "auto", s_free_at_start / 1024);
    /* For bug reports: what the overlay had to work with on this console. */
    snprintf(s_mem_head, sizeof(s_mem_head), "%s\n%s\ncapture block %zu KB, framebuffers %zu KB, reserve %zu KB\n",
             s_res_note, s_ocr_model_note, kCaptureBlockBytes / 1024, framebuffer_cost(pw, ph) / 1024, kReserve / 1024);
    FILE *f = fopen("sdmc:/switch/safir/memory.log", "w");
    if (f) {
        fputs(s_mem_head, f);
        fclose(f);
    }
}

/* First model file found under any data folder; size in *bytes. */
static bool find_model_file(const char *const *names, size_t n_names, char *path, size_t cap, size_t *bytes) {
    for (size_t d = 0; d < ARRAY_LEN(s_asset_dirs); ++d) {
        for (size_t n = 0; n < n_names; ++n) {
            snprintf(path, cap, "%s%s", s_asset_dirs[d], names[n]);
            struct stat st;
            if (stat(path, &st) == 0 && st.st_size > 80) {
                *bytes = (size_t)st.st_size;
                return true;
            }
        }
    }
    path[0] = '\0';
    return false;
}

/* Runs in initServices FIRST, before every other long-lived block and the framebuffers:
 * takes one heap block sized to the chosen model and hands it to POOL_OCR_WEIGHTS, so the
 * model never fragments the heap and .bss holds no model memory at all.
 * auto takes Normal only when, after it, the heap still has room for the page cache, the
 * glyph cache, a 640x384 popup, the capture block and the reserve (as choose_popup_size
 * counts them); otherwise Small. normal takes Normal whenever it can be allocated (the
 * popup then shrinks to what is left). */
static void choose_ocr_model(size_t later_blocks) {
    char npath[96], spath[96];
    size_t nbytes = 0, sbytes = 0;
    bool have_n = s_ocr_model_pref != OCR_MODEL_SMALL &&
                  find_model_file(s_ocr_normal_names, ARRAY_LEN(s_ocr_normal_names), npath, sizeof(npath), &nbytes);
    bool have_s = find_model_file(s_ocr_names, ARRAY_LEN(s_ocr_names), spath, sizeof(spath), &sbytes);
    const size_t free0 = largest_free_block();
    const size_t after = later_blocks + framebuffer_cost(640, 384) + kCaptureBlockBytes + 416 * 1024;
    const char *why = "";
    struct Cand { const char *path; size_t bytes; const char *label; } cands[2];
    int nc = 0;
    if (have_n) {
        if (s_ocr_model_pref == OCR_MODEL_NORMAL || nbytes + kOcrContextBytes + after <= free0) {
            cands[nc++] = { npath, nbytes, "Normal" };
        } else {
            why = ", Normal skipped: not enough memory (raise the overlay heap to 6 MB or set ocr_model=normal)";
        }
    } else if (s_ocr_model_pref != OCR_MODEL_SMALL) {
        why = ", no VOCR6.2_N_w8g2.vocr found";
    }
    if (have_s) cands[nc++] = { spath, sbytes, "Small" };
    for (int i = 0; i < nc; ++i) {
        size_t cap = (cands[i].bytes + kOcrContextBytes + 15) & ~(size_t)15;
        void *mem = aligned_alloc(16, cap);
        if (!mem) continue;
        if (!safir_pool_attach(POOL_OCR_WEIGHTS, mem, cap)) { free(mem); continue; }
        copy_utf8_trunc(s_ocr_model_path, sizeof(s_ocr_model_path), cands[i].path);
        s_ocr_model_label = cands[i].label;
        snprintf(s_ocr_model_note, sizeof(s_ocr_model_note), "OCR model %s (%zu KB, ocr_model=%s)%s", cands[i].label,
                 cands[i].bytes / 1024,
                 s_ocr_model_pref == OCR_MODEL_SMALL ? "small" : s_ocr_model_pref == OCR_MODEL_NORMAL ? "normal" : "auto",
                 (i > 0 && have_n && s_ocr_model_pref != OCR_MODEL_SMALL && !why[0])
                     ? ", Normal did not fit in memory" : why);
        return;
    }
    snprintf(s_ocr_model_note, sizeof(s_ocr_model_note), nc ? "no memory for the OCR model (%zu KB free)"
                                                             : "no OCR model in sdmc:/switch/safir/ (%zu KB free)",
             free0 / 1024);
}

/* Lowest "largest free block" seen at a scan's peak, written to memory.log when it drops,
 * so a report shows how close the overlay came to running out (not just the start). */
static size_t s_low_free = 0;
static void note_heap_low(const char *when) {
    size_t f = largest_free_block();
    if (s_low_free != 0 && f >= s_low_free) return;
    s_low_free = f;
    FILE *fp = fopen("sdmc:/switch/safir/memory.log", "w");
    if (fp) {
        fprintf(fp, "%slowest free during scans: %zu KB (scan #%u, %s)\n", s_mem_head, f / 1024, (unsigned)s_scan_no,
                when);
        fclose(fp);
    }
}

/* The capture block exists only between acquire and release, inside run_scan_now(). */
static bool acquire_capture_buffers(void) {
    if (!s_capture_block) s_capture_block = (uint8_t *)malloc(kCaptureBlockBytes);
    if (!s_capture_block) {
        snprintf(s_heap_note, sizeof(s_heap_note), "Out of memory: a scan needs %zu KB, largest free block %zu KB (%s)",
                 kCaptureBlockBytes / 1024, largest_free_block() / 1024, s_res_note);
        return false;
    }
    safir_set_frame_crop_buffer(s_capture_block, kFrameCropBytes);
    s_heap_note[0] = '\0';
    return true;
}

static void release_capture_buffers(void) {
    safir_capture_set_jpeg_buffer(nullptr, 0);
    safir_set_frame_crop_buffer(nullptr, 0);
    free(s_capture_block);
    s_capture_block = nullptr;
}

static void load_assets_if_needed(void) {
    safir_engine_lock();
    if (!s_dict_loaded) load_dictionary();
    if (!s_ocr_loaded) load_ocr_model();
    s_assets_probed = true;
    safir_engine_unlock();
}

static void set_pill(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_pill(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_pill, sizeof(s_pill), fmt, ap);
    va_end(ap);
}

/* Frees everything the previous scan and popup held, so the capture block (742 KB) finds
 * one contiguous free region: glyph bitmaps, word list, lines. */
static void release_scan_state(void);

/* Recognized lines -> s_scan_lines, line boxes and dictionary words (both engines).
 * Called with the engine lock held. Returns the number of characters. */
static unsigned collect_scan_words(const SafirOcrResult &ocr_res, unsigned nlines) {
    unsigned total_chars = 0;
    s_scan_lines.clear();
    for (unsigned l = 0; l < nlines; ++l) {
        const SafirTextLine *tl = &ocr_res.lines[l];
        total_chars += tl->char_count;
        {
            char txt[SAFIR_MAX_CHARS_PER_LINE * 4 + 1];
            safir_text_line_to_utf8(tl, txt, sizeof(txt));
            evlog("scan #%u: line %u \"%s\"", (unsigned)s_scan_no, l, txt);
        }
        std::vector<uint32_t> cps(tl->char_count);
        s32 lb[4] = { INT32_MAX, INT32_MAX, 0, 0 };
        for (uint16_t k = 0; k < tl->char_count; ++k) {
            cps[k] = tl->chars[k].codepoint;
            const SafirRect &b = tl->chars[k].screen_box;
            lb[0] = std::min<s32>(lb[0], b.x);
            lb[1] = std::min<s32>(lb[1], b.y);
            lb[2] = std::max<s32>(lb[2], b.x + b.w);
            lb[3] = std::max<s32>(lb[3], b.y + b.h);
        }
        s_scan_lines.push_back(std::move(cps));
        if (tl->char_count > 0 && s_line_box_n < SAFIR_MAX_LINES_PER_BOX) {
            memcpy(s_line_box[s_line_box_n++], lb, sizeof(lb));
        }
        if (!s_dict_loaded) continue;
        /* Yomitan-style scan: longest match (with deinflection) at each position; a
         * position without a match (punctuation, names, OCR noise) is skipped. */
        for (uint16_t i = 0; i < tl->char_count && s_scan_words.size() < kMaxScanWords;) {
            SafirLookupResult lres;
            memset(&lres, 0, sizeof(lres));
            bool hit = safir_dict_scan_longest(&s_switch_dict, tl, i, &lres) && lres.found && lres.matched_chars > 0;
            if (!hit) { ++i; continue; }
            uint16_t m = lres.matched_chars;
            if (m > tl->char_count - i) m = (uint16_t)(tl->char_count - i);
            SafirTextLine piece;
            memset(&piece, 0, sizeof(piece));
            memcpy(piece.chars, &tl->chars[i], (size_t)m * sizeof(SafirCharBox));
            piece.char_count = m;
            char surface[SAFIR_MAX_CHARS_PER_LINE * 4 + 1];
            safir_text_line_to_utf8(&piece, surface, sizeof(surface));
            ScanWord w{ surface, lres.headword, l, i, m, INT32_MAX, INT32_MAX, 0, 0 };
            for (uint16_t k = 0; k < m; ++k) {
                const SafirRect &b = tl->chars[i + k].screen_box;
                w.x0 = std::min<s32>(w.x0, b.x);
                w.y0 = std::min<s32>(w.y0, b.y);
                w.x1 = std::max<s32>(w.x1, b.x + b.w);
                w.y1 = std::max<s32>(w.y1, b.y + b.h);
            }
            s_scan_words.push_back(std::move(w));
            i = (uint16_t)(i + m);
        }
    }

    return total_chars;
}

/* Status pill for a scan that found text. */
static void set_result_pill(unsigned nlines, const char *prefix, const char *timing) {
    if (!s_dict_loaded) {
        set_pill("%s%u lines, dictionary not loaded", prefix, nlines);
        copy_utf8_trunc(s_detail, sizeof(s_detail), s_dict_status);
        return;
    }
    set_pill("%s%u lines · %zu words · %s", prefix, nlines, s_scan_words.size(), timing);
    if (s_scan_words.empty()) {
        copy_utf8_trunc(s_detail, sizeof(s_detail), "Text found, but no dictionary words in it.");
    }
}

static void run_scan_now_inner(void);

static void start_scan_after(int frames);

/* Runs on the UI thread after the layer has been transparent for a few frames. */
static void run_scan_now(void) {
    load_assets_if_needed();
    release_scan_state();
    s_detail[0] = '\0';
    ++s_scan_no;
    evlog("scan #%u: capture starts (local)", (unsigned)s_scan_no);
    if (!s_ocr_loaded) { /* a failed earlier load is retried */
        safir_engine_lock();
        load_ocr_model();
        safir_engine_unlock();
    }
    if (!acquire_capture_buffers()) {
        set_pill("Not enough memory to scan");
        snprintf(s_detail, sizeof(s_detail), "%s\nIn Ultrahand, raise the overlay memory (nx-ovlloader heap) to 6 or 8 MB, "
                 "or set popup_width=576 in sdmc:/config/safir/config.ini.", s_heap_note);
        return;
    }
    note_heap_low("local OCR, capture buffers held");
    run_scan_now_inner();
    release_capture_buffers();
    evlog("scan #%u: done, %zu words, pill \"%s\"", (unsigned)s_scan_no, s_scan_words.size(), s_pill);
}

static void run_scan_now_inner(void) {

    if (R_FAILED(s_rc_capssc)) {
        set_pill("caps:sc init failed rc=0x%x", (unsigned)s_rc_capssc);
        return;
    }
    if (!s_ocr_loaded) {
        set_pill("OCR model not loaded");
        snprintf(s_detail, sizeof(s_detail), "%s\n%s", s_ocr_status, s_dict_status);
        return;
    }

    SafirRect rect = s_scan_rect;
    uint16_t crop_w = (uint16_t)((rect.w + 1) / 2);
    uint16_t crop_h = (uint16_t)((rect.h + 1) / 2);
    size_t num_pixels = (size_t)crop_w * crop_h;

    u64 scan_t0 = armGetSystemTick();
    safir_engine_lock();

    uint8_t *crop_gray = safir_acquire_crop(num_pixels);
    if (!crop_gray) {
        safir_engine_unlock();
        set_pill("No memory for the screen crop");
        snprintf(s_detail, sizeof(s_detail),
                 "A %ux%u crop needs %zu B: heap buffer %s, scratch cap %zu B. "
                 "Set a smaller region in sdmc:/config/safir/games.ini.",
                 crop_w, crop_h, num_pixels, "too small",
                 safir_pool_capacity(POOL_OCR_SCRATCH));
        return;
    }

    /* The JPEG goes into the model's memory, which is re-read right after decoding. */
    size_t lent_bytes = 0;
    void *jpeg_buf = safir_ocr_vocr_lend_weights(s_switch_ocr, &lent_bytes);
    safir_capture_set_jpeg_buffer(jpeg_buf, lent_bytes);
    Result rc = safir_capture_crop_grayscale(crop_gray, rect, 100000000ULL);

    /* Two-pass scan while the JPEG still sits in the model's memory: lines are found on the
     * half-resolution frame just decoded, then each line is decoded again from the JPEG at
     * full resolution into the same buffer. Small game text (18 px) used to be read at 9 px. */
    SafirScanPlan plan;
    memset(&plan, 0, sizeof(plan));
    int plan_rc = -100;
    const uint8_t *jpeg = nullptr;
    size_t jpeg_size = 0;
    if (R_SUCCEEDED(rc) && safir_capture_last_jpeg(&jpeg, &jpeg_size)) {
        /* Which picture this scan read: a fingerprint in events.log on every scan (the same
         * value twice in a row = the same screenshot), and with save_scans=1 the file itself
         * for the last 4 scans (scan_1.jpg .. scan_4.jpg). Investigates "reopening with
         * ZL+ZR shows the previous line": a stale capture, or a stale popup. */
        uint32_t fp = 2166136261u; /* FNV-1a */
        for (size_t i = 0; i < jpeg_size; ++i) fp = (fp ^ jpeg[i]) * 16777619u;
        if (s_save_scans) {
            char path[64];
            snprintf(path, sizeof(path), "sdmc:/switch/safir/scan_%u.jpg", (unsigned)((s_scan_no - 1) % 4 + 1));
            FILE *jf = fopen(path, "wb");
            if (jf) {
                fwrite(jpeg, 1, jpeg_size, jf);
                fclose(jf);
            }
            evlog("scan #%u: screenshot %zu B, fingerprint %08x, saved as %s", (unsigned)s_scan_no, jpeg_size,
                  (unsigned)fp, path);
        } else {
            evlog("scan #%u: screenshot %zu B, fingerprint %08x", (unsigned)s_scan_no, jpeg_size, (unsigned)fp);
        }
        size_t cap = (crop_gray == s_capture_block) ? kFrameCropBytes : num_pixels;
        plan_rc = safir_scan_prepare(jpeg, jpeg_size, rect, crop_gray, cap, true, &plan);
        /* A plan that failed part-way may have overwritten the half-resolution frame:
         * decode it again now, while the JPEG still exists, for the single-pass fallback. */
        if (plan_rc != 0) (void)safir_jpeg_crop_gray(jpeg, jpeg_size, rect, crop_gray, nullptr, nullptr);
    }
    safir_capture_set_jpeg_buffer(nullptr, 0);
    u64 reload_t0 = armGetSystemTick();
    if (jpeg_buf && !safir_ocr_vocr_restore_weights(s_switch_ocr)) {
        safir_engine_unlock();
        set_pill("OCR model could not be re-read");
        snprintf(s_detail, sizeof(s_detail), "%s", safir_ocr_vocr_last_error());
        return;
    }
    s_reload_ms = (u32)(armTicksToNs(armGetSystemTick() - reload_t0) / 1000000ULL);
    if (R_FAILED(rc)) {
        safir_engine_unlock();
        s_capture_failures++;
        /* Every attempt's code (JPEG on two layer stacks, then raw), not just the last. */
        set_pill("Capture failed: %s", safir_capture_last_diag());
        snprintf(s_detail, sizeof(s_detail), "Capture failed (try %d): %s\n%s\n%s\n%s\nY retries.", s_capture_failures,
                 safir_capture_last_diag(), s_heap_note, s_res_note, s_region_status);
        return;
    }
    s_capture_failures = 0;

    SafirOcrResult &ocr_res = s_ocr_res; /* shared by both engines: one 16 KB buffer in .bss */
    memset(&ocr_res, 0, sizeof(ocr_res));
    bool recognized = false;
    if (plan_rc == 0) {
        recognized = safir_ocr_vocr_recognize_plan(s_switch_ocr, crop_gray, &plan, &ocr_res);
        write_scan_log(&plan, recognized ? ocr_res.line_count : 0u);
    } else {
        /* Raw-capture fallback (no JPEG) or a failed plan: the single half-resolution pass. */
        safir_crop_auto_invert(crop_gray, crop_w, crop_h);
        recognized = s_switch_ocr && s_switch_ocr->recognize &&
                     s_switch_ocr->recognize(s_switch_ocr, crop_gray, crop_w, crop_h, rect, &ocr_res);
    }

    unsigned nlines = recognized ? ocr_res.line_count : 0;
    unsigned total_chars = collect_scan_words(ocr_res, nlines);
    safir_engine_unlock();
    s_last_scan_ms = (u32)(armTicksToNs(armGetSystemTick() - scan_t0) / 1000000ULL);

    if (total_chars == 0) {
        if (plan_rc != 0) dump_debug_capture(crop_gray, crop_w, crop_h); /* else see debug_scan.txt */
        set_pill("No text found (%u ms)", (unsigned)s_last_scan_ms);
        snprintf(s_detail, sizeof(s_detail), "%s\n%s\nSaved debug_screen.jpg and debug_crop.pgm in sdmc:/switch/safir/",
                 safir_ocr_vocr_last_error(), s_region_status);
        return;
    }
    char timing[48];
    snprintf(timing, sizeof(timing), "%u ms (model %u)", (unsigned)s_last_scan_ms, (unsigned)s_reload_ms);
    set_result_pill(nlines, safir_ocr_vocr_is_placeholder() ? "[FAKE OCR] " : "", timing);
}

/* sdmc:/config/safir/config.ini, [safir] scan_combo=ZL+ZR (libtesla key names: L R ZL ZR
 * LS RS DUP DDOWN DLEFT DRIGHT A B X Y MINUS PLUS). The combo opens the overlay, which
 * scans at once, and hides it again. Written with the default on first start. */
static const char *kConfigPath = "sdmc:/config/safir/config.ini";
static const char *kDefaultScanCombo = "ZL+ZR";
static char s_scan_combo_name[48] = "ZL+ZR";

static void load_scan_combo(void) {
    std::string combo = kDefaultScanCombo;
    FILE *f = fopen(kConfigPath, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            std::string key = line, val = eq + 1;
            auto trim = [](std::string &t) {
                while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
                while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
            };
            trim(key);
            trim(val);
            if (key == "scan_combo" && !val.empty()) combo = val;
            if (key == "save_scans") s_save_scans = (val == "1" || val == "on" || val == "true");
            if (key == "ocr_model") {
                s_ocr_model_pref = val == "small" ? OCR_MODEL_SMALL : val == "normal" ? OCR_MODEL_NORMAL : OCR_MODEL_AUTO;
            }
            if (key == "popup_width") {
                int wpx = atoi(val.c_str());
                if (wpx >= 480 && wpx <= 1024) s_forced_popup_width = wpx & ~15;
            }
        }
        fclose(f);
    } else {
        mkdir("sdmc:/config", 0777);
        mkdir("sdmc:/config/safir", 0777);
        f = fopen(kConfigPath, "w");
        if (f) {
            fprintf(f, "[safir]\n; Buttons pressed together to scan the screen (opens/closes safir).\n"
                       "; Names: L R ZL ZR LS RS DUP DDOWN DLEFT DRIGHT A B X Y MINUS PLUS\n"
                       "scan_combo=%s\n"
                       "; Popup width in screen pixels: auto (widest that fits in memory) or 480-1024.\n"
                       "; The popup is drawn 1:1 (sharp); 384 px tall. 640 = 0.98 MB, 768 = 1.18 MB.\n"
                       "popup_width=auto\n"
                       "; OCR model: auto (Normal when the overlay memory has room, else Small), small or normal.\n"
                       "; Normal (VOCR6.2_N_w8g2.vocr) makes about half the mistakes and needs ~370 KB more memory.\n"
                       "ocr_model=auto\n"
                       "; 1: keep each scan's screenshot as scan_1..4.jpg in sdmc:/switch/safir/ (for bug reports).\n"
                       "save_scans=0\n", kDefaultScanCombo);
            fclose(f);
        }
    }
    u64 keys = tsl::hlp::comboStringToKeys(combo);
    if (keys == 0) { /* unknown names: fall back rather than lose the shortcut */
        combo = kDefaultScanCombo;
        keys = tsl::hlp::comboStringToKeys(combo);
    }
    tsl::cfg::extraLaunchCombo = keys;
    copy_utf8_trunc(s_scan_combo_name, sizeof(s_scan_combo_name), combo.c_str());
}

/* ------------------------------------------------------------------------- */
/* Popup UI                                                                  */
/* ------------------------------------------------------------------------- */
/*
 * libtesla's layer is popup-sized (tsl::cfg::FloatingLayer) and drawn at one framebuffer
 * pixel per screen pixel, then moved next to the selected word with setLayerPos(). Every
 * size below is in screen pixels. Outside the drawn popup the layer is transparent.
 *
 * Controls: D-pad left/right word, up/down line, L/R previous/next dictionary entry,
 * ZL/ZR or right stick scroll, A show/hide, Y rescan, B close, + exit to the Tesla menu.
 */
static const tsl::Color kBg      = { 0x1, 0x1, 0x2, 0xE };
static const tsl::Color kStrip   = { 0x0, 0x0, 0x1, 0xF };
static const tsl::Color kAccent  = { 0x3, 0x9, 0xF, 0xF };
static const tsl::Color kText    = { 0xF, 0xF, 0xF, 0xF };
static const tsl::Color kGloss   = { 0xE, 0xE, 0xE, 0xF };
static const tsl::Color kDim     = { 0x9, 0x9, 0xA, 0xF };
static const tsl::Color kReading = { 0xA, 0xD, 0xF, 0xF };
static const tsl::Color kRule    = { 0x9, 0xD, 0x9, 0xF };
static const tsl::Color kNone    = { 0x0, 0x0, 0x0, 0x0 };

/* Three sizes only: every (character, size) pair is a separate cached bitmap. */
static const float kHeadFont = 26.0f, kBodyFont = 17.0f, kSmallFont = 14.0f;
static const float kStripFont = kBodyFont, kReadFont = kBodyFont, kGlossFont = kBodyFont, kRuleFont = kSmallFont;
static const s32 kGlossLine = 23, kPad = 12;

static int s_sel = -1;            /* selected word, -1 = none */
static bool s_popup = false;      /* popup visible */
static int s_scroll = 0;          /* first glossary line shown */
static bool s_scan_on_show = true;

/* Every dictionary entry for the selected word (safir_dict_lookup_all), read on selection. */
struct EntryView {
    std::string headword, reading, rule, gloss, surface;
};
static std::vector<EntryView> s_entries;
static int s_entry = 0;           /* entry shown */
static int s_entries_for = -1;    /* word index s_entries belongs to */
static const size_t kMaxEntries = 24;

static void load_entries(int word) {
    s_entries.clear();
    s_entry = 0;
    s_scroll = 0;
    s_entries_for = word;
    if (word < 0 || word >= (int)s_scan_words.size() || !s_dict_loaded) return;
    const ScanWord &w = s_scan_words[word];
    if (w.line >= s_scan_lines.size()) return;
    const std::vector<uint32_t> &cps = s_scan_lines[w.line];

    static SafirTextLine line; /* large: keep it off the stack */
    memset(&line, 0, sizeof(line));
    line.char_count = (uint16_t)std::min<size_t>(cps.size(), SAFIR_MAX_CHARS_PER_LINE);
    for (uint16_t k = 0; k < line.char_count; ++k) line.chars[k].codepoint = cps[k];

    static SafirDictMatch matches[kMaxEntries];
    static char gloss[1536];
    safir_engine_lock();
    size_t n = safir_dict_lookup_all(&s_switch_dict, &line, w.char_start, matches, kMaxEntries);
    for (size_t i = 0; i < n; ++i) {
        char hw[64], rd[64];
        if (!safir_dict_read_entry(&s_switch_dict, &matches[i], hw, sizeof(hw), rd, sizeof(rd), gloss, sizeof(gloss)))
            continue;
        SafirTextLine piece;
        memset(&piece, 0, sizeof(piece));
        piece.char_count = std::min<uint16_t>(matches[i].matched_chars, (uint16_t)(line.char_count - w.char_start));
        memcpy(piece.chars, &line.chars[w.char_start], piece.char_count * sizeof(SafirCharBox));
        char surface[SAFIR_MAX_CHARS_PER_LINE * 4 + 1];
        safir_text_line_to_utf8(&piece, surface, sizeof(surface));
        s_entries.push_back({ hw, strcmp(rd, hw) == 0 ? std::string() : std::string(rd), matches[i].rule, gloss, surface });
    }
    safir_engine_unlock();
}

static void select_word(int idx) {
    if (s_scan_words.empty()) { s_sel = -1; s_popup = false; return; }
    int n = (int)s_scan_words.size();
    s_sel = ((idx % n) + n) % n;
    s_popup = true;
    if (s_entries_for != s_sel) load_entries(s_sel);
    s_scroll = 0;
}

/* Nearest word on the previous (dir -1) / next (dir +1) line, by horizontal centre. */
static void select_line(int dir) {
    if (s_sel < 0) { select_word(0); return; }
    const ScanWord &cur = s_scan_words[s_sel];
    int target_line = -1;
    for (const ScanWord &w : s_scan_words) {
        int l = (int)w.line;
        if (dir > 0 && l > (int)cur.line && (target_line < 0 || l < target_line)) target_line = l;
        if (dir < 0 && l < (int)cur.line && (target_line < 0 || l > target_line)) target_line = l;
    }
    if (target_line < 0) return;
    s32 cx = (cur.x0 + cur.x1) / 2;
    int best = -1;
    s32 best_d = INT32_MAX;
    for (size_t i = 0; i < s_scan_words.size(); ++i) {
        const ScanWord &w = s_scan_words[i];
        if ((int)w.line != target_line) continue;
        s32 d = std::abs((w.x0 + w.x1) / 2 - cx);
        if (d < best_d) { best_d = d; best = (int)i; }
    }
    if (best >= 0) select_word(best);
}

static void step_entry(int dir) {
    if (s_entries.empty()) return;
    int n = (int)s_entries.size();
    s_entry = ((s_entry + dir) % n + n) % n;
    s_scroll = 0;
}

static size_t utf8_len(unsigned char c) {
    return (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 4;
}

/* Real advance of a string with libtesla's fonts (transparent colour = measure only). */
static s32 text_w(tsl::gfx::Renderer *r, const char *t, float size) {
    if (!t || !*t) return 0;
    return (s32)r->drawString(t, false, 0, 0, size, kNone).first;
}

/* "1) a; b 2) c" -> one sense per line. */
static std::string senses_on_lines(const std::string &gloss) {
    std::string out;
    out.reserve(gloss.size() + 16);
    for (size_t i = 0; i < gloss.size(); ++i) {
        if (gloss[i] == ' ' && i + 2 < gloss.size() && gloss[i + 1] >= '2' && gloss[i + 1] <= '9' &&
            (gloss[i + 2] == ')' || (i + 3 < gloss.size() && isdigit((unsigned char)gloss[i + 2]) && gloss[i + 3] == ')'))) {
            out += '\n';
            continue;
        }
        out += gloss[i];
    }
    return out;
}

/* Word wrap with measured widths: English breaks at spaces, CJK anywhere. Continuation
 * lines of a numbered sense are indented. */
static std::vector<std::string> wrap_measured(tsl::gfx::Renderer *r, const std::string &text, float size, s32 max_w) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string para = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        std::string cur;
        s32 cur_w = 0;
        size_t last_space = std::string::npos; /* byte index in cur */
        bool first = true;
        const s32 indent_w = text_w(r, "    ", size);
        for (size_t i = 0; i < para.size();) {
            size_t n = utf8_len((unsigned char)para[i]);
            std::string ch = para.substr(i, n);
            s32 cw = text_w(r, ch.c_str(), size);
            s32 limit = first ? max_w : max_w - indent_w;
            if (cur_w + cw > limit && !cur.empty()) {
                std::string rest;
                if (ch != " " && last_space != std::string::npos && last_space > 0) {
                    rest = cur.substr(last_space + 1);
                    cur.erase(last_space);
                }
                out.push_back(first ? cur : "    " + cur);
                first = false;
                cur = rest;
                cur_w = text_w(r, cur.c_str(), size);
                last_space = std::string::npos;
                if (ch == " ") { i += n; continue; }
            }
            if (ch == " ") last_space = cur.size();
            cur += ch;
            cur_w += cw;
            i += n;
        }
        if (!cur.empty() || first) out.push_back(first ? cur : "    " + cur);
    }
    return out;
}

/* Popup layout, recomputed only when the word, entry or popup width changes. */
struct PopupLayout {
    int word = -2, entry = -2, width = 0;
    std::vector<std::string> gloss;
    bool has_rule = false;
};
static PopupLayout s_layout;

/* Screen-space rectangle of the popup this frame (for touch). */
static s32 s_pop_x = 0, s_pop_y = 0, s_pop_w = 0, s_pop_h = 0;

/* Moving the layer takes effect at once, while the frame on screen is still the one drawn
 * for the old position: for one frame the old popup showed at the new place (the doubled
 * popup in the console video). So a move first presents one transparent frame, then moves
 * the layer, then draws. Returns false for the frame that must stay empty. */
static bool s_move_pending = false;

static bool move_layer(tsl::gfx::Renderer *r, s32 x, s32 y) {
    const s32 W = tsl::cfg::FramebufferWidth, H = tsl::cfg::FramebufferHeight;
    x = std::clamp<s32>(x, 0, 1280 - W);
    y = std::clamp<s32>(y, 0, 720 - H);
    const u16 vx = (u16)(x * 3 / 2), vy = (u16)(y * 3 / 2);
    if (vx == tsl::cfg::LayerPosX && vy == tsl::cfg::LayerPosY) {
        s_move_pending = false;
        return true;
    }
    if (!s_move_pending) {
        s_move_pending = true; /* this frame: empty */
        return false;
    }
    s_move_pending = false;    /* the empty frame is on screen now: safe to move */
    r->setLayerPos(vx, vy);
    return true;
}

/* The text block a word belongs to: its line plus the lines directly above and below it
 * (overlapping horizontally, at most about one line apart). The popup goes outside it. */
static void word_text_block(const ScanWord &w, s32 *bx0, s32 *by0, s32 *bx1, s32 *by1) {
    *bx0 = w.x0; *by0 = w.y0; *bx1 = w.x1; *by1 = w.y1;
    if (w.line < s_line_box_n) {
        const s32 *l = s_line_box[w.line];
        *bx0 = std::min(*bx0, l[0]); *by0 = std::min(*by0, l[1]);
        *bx1 = std::max(*bx1, l[2]); *by1 = std::max(*by1, l[3]);
    }
    bool grew = true;
    while (grew) {
        grew = false;
        for (unsigned k = 0; k < s_line_box_n; ++k) {
            const s32 *l = s_line_box[k];
            if (l[0] >= *bx0 && l[1] >= *by0 && l[2] <= *bx1 && l[3] <= *by1) continue; /* inside */
            const s32 lh = std::max<s32>(8, l[3] - l[1]);
            bool over_x = l[0] < *bx1 && l[2] > *bx0;
            bool near_y = l[1] <= *by1 + lh && l[3] >= *by0 - lh;
            if (over_x && near_y) {
                *bx0 = std::min(*bx0, l[0]); *by0 = std::min(*by0, l[1]);
                *bx1 = std::max(*bx1, l[2]); *by1 = std::max(*by1, l[3]);
                grew = true;
            }
        }
    }
}

static s32 layer_x(void) { return (s32)tsl::cfg::LayerPosX * 2 / 3; }
static s32 layer_y(void) { return (s32)tsl::cfg::LayerPosY * 2 / 3; }

class PopupScreen : public tsl::elm::Element {
public:
    virtual void draw(tsl::gfx::Renderer *r) override {
        r->clearScreen();
        s_pop_w = s_pop_h = 0;
        if (s_capture_countdown > 0) return; /* keep the layer empty: it is in the screenshot */

        if (s_popup && s_sel >= 0 && s_sel < (int)s_scan_words.size()) {
            draw_popup(r, s_scan_words[s_sel]);
        } else {
            draw_status(r);
        }
    }

    virtual void layout(u16 parentX, u16 parentY, u16 parentWidth, u16 parentHeight) override {
        this->setBoundaries(parentX, parentY, parentWidth, parentHeight);
    }

    virtual bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY, s32 prevX, s32 prevY, s32 initialX,
                         s32 initialY) override {
        (void)prevX; (void)prevY; (void)initialX; (void)initialY;
        if (event != tsl::elm::TouchEvent::Touch || s_capture_countdown > 0) return true;
        /* Touch coordinates are screen pixels, like the word boxes. */
        if (s_pop_w > 0 && currX >= s_pop_x && currX < s_pop_x + s_pop_w && currY >= s_pop_y &&
            currY < s_pop_y + s_pop_h) {
            step_entry(+1); /* tap the popup: next entry */
            return true;
        }
        const s32 pad = 10;
        int hit = -1;
        for (size_t i = 0; i < s_scan_words.size(); ++i) {
            const ScanWord &w = s_scan_words[i];
            if (currX >= w.x0 - pad && currX <= w.x1 + pad && currY >= w.y0 - pad && currY <= w.y1 + pad) {
                hit = (int)i;
                if (currX >= w.x0 && currX <= w.x1) break; /* prefer the box the finger is really in */
            }
        }
        if (hit >= 0) select_word(hit);
        else s_popup = false;
        return true;
    }

private:
    /* Status pill (and the explanation box when a scan found nothing), top centre. */
    void draw_status(tsl::gfx::Renderer *r) {
        const s32 W = tsl::cfg::FramebufferWidth;
        if (!move_layer(r, (1280 - W) / 2, 6)) return;
        s32 y = 0;
        if (s_pill[0]) {
            s32 pw = std::min<s32>(W, text_w(r, s_pill, kSmallFont) + 2 * kPad);
            s32 x = (W - pw) / 2;
            r->drawRect(x, y, pw, 30, a(kBg));
            r->drawString(s_pill, false, x + kPad, y + 21, kSmallFont, a(kText), W - 2 * kPad);
            y += 34;
        }
        if (s_scan_words.empty() && s_detail[0]) {
            std::vector<std::string> lines = wrap_measured(r, s_detail, kSmallFont, W - 2 * kPad);
            const s32 H = tsl::cfg::FramebufferHeight;
            s32 max_lines = (H - y - 2 * kPad) / 20;
            s32 shown = std::min<s32>((s32)lines.size(), max_lines);
            r->drawRect(0, y, W, shown * 20 + 2 * kPad - 4, a(kBg));
            for (s32 i = 0; i < shown; ++i)
                r->drawString(lines[i].c_str(), false, kPad, y + kPad + 14 + i * 20, kSmallFont, a(kGloss));
            y += shown * 20 + 2 * kPad;
        }
        char hint[160];
        snprintf(hint, sizeof(hint), " rescan    word    game (%s scans)    exit",
                 s_scan_combo_name);
        s32 hw = std::min<s32>(W, text_w(r, hint, kSmallFont) + 2 * kPad);
        r->drawRect((W - hw) / 2, y, hw, 24, a(kBg));
        r->drawString(hint, false, (W - hw) / 2 + kPad, y + 17, kSmallFont, a(kDim));
    }

    /* The OCR line with the selected word highlighted, cut to fit around it. */
    void draw_context(tsl::gfx::Renderer *r, const ScanWord &w, s32 x, s32 y, s32 width) {
        r->drawRect(x, y, width, 32, a(kStrip));
        if (w.line >= s_scan_lines.size()) return;
        const std::vector<uint32_t> &cps = s_scan_lines[w.line];
        auto cp_str = [](uint32_t cp) {
            char b[5] = { 0 };
            if (cp < 0x80) { b[0] = (char)cp; }
            else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); }
            else { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); }
            return std::string(b);
        };
        const s32 avail = width - 2 * kPad;
        std::vector<s32> cw(cps.size());
        for (size_t i = 0; i < cps.size(); ++i) cw[i] = text_w(r, cp_str(cps[i]).c_str(), kStripFont);
        size_t a0 = w.char_start, a1 = std::min<size_t>(w.char_start + w.char_count, cps.size());
        s32 used = 0;
        for (size_t i = a0; i < a1; ++i) used += cw[i];
        size_t b = a0, e = a1; /* grow around the word, a little more to the right */
        bool grew = true;
        while (grew) {
            grew = false;
            if (e < cps.size() && used + cw[e] <= avail) { used += cw[e++]; grew = true; }
            if (e < cps.size() && used + cw[e] <= avail) { used += cw[e++]; grew = true; }
            if (b > 0 && used + cw[b - 1] <= avail) { used += cw[--b]; grew = true; }
        }
        s32 cx = x + kPad;
        for (size_t i = b; i < e; ++i) {
            bool in_word = i >= a0 && i < a1;
            if (in_word) {
                r->drawRect(cx, y + 3, cw[i], 26, a(kAccent));
            }
            r->drawString(cp_str(cps[i]).c_str(), false, cx, y + 23, kStripFont, a(in_word ? kText : kDim));
            cx += cw[i];
        }
    }

    void draw_popup(tsl::gfx::Renderer *r, const ScanWord &w) {
        const s32 W = tsl::cfg::FramebufferWidth, H = tsl::cfg::FramebufferHeight;
        if (s_entries_for != s_sel) load_entries(s_sel);
        const EntryView *e = s_entries.empty() ? nullptr : &s_entries[std::clamp(s_entry, 0, (int)s_entries.size() - 1)];

        if (s_layout.word != s_sel || s_layout.entry != s_entry || s_layout.width != W) {
            s_layout.word = s_sel;
            s_layout.entry = s_entry;
            s_layout.width = W;
            s_layout.gloss = e ? wrap_measured(r, senses_on_lines(e->gloss), kGlossFont, W - 2 * kPad - 6)
                               : std::vector<std::string>{ "No dictionary entry." };
            s_layout.has_rule = e && (!e->rule.empty() || e->surface != e->headword);
        }

        /* Heights: strip, header, rule line, glossary, footer. */
        const s32 fixed = 32 + 8 + 34 + (s_layout.has_rule ? 22 : 0) + 8 + 10 + 26;
        const s32 total = (s32)s_layout.gloss.size();
        const s32 max_lines = std::max<s32>(1, (H - fixed) / kGlossLine);

        /* Outside the word's whole text block, so the rest of the dialogue stays readable:
         * below it when the block is in the top half, above it otherwise, whichever side has
         * room for the header and a few glossary lines. Only when neither side has room does
         * the popup sit next to the word itself (covering other lines of the block). */
        s32 bx0, by0, bx1, by1;
        word_text_block(w, &bx0, &by0, &bx1, &by1);
        const s32 want = fixed + std::min<s32>(total, 3) * kGlossLine;
        s32 anchor_top = by0, anchor_bottom = by1;
        if (std::max(720 - (by1 + 8), by0 - 8) < want) { anchor_top = w.y0; anchor_bottom = w.y1; }
        s32 room_below = 720 - (anchor_bottom + 8), room_above = anchor_top - 8;
        bool prefer_above = (anchor_top + anchor_bottom) / 2 > 360;
        bool above = prefer_above ? (room_above >= want || room_above > room_below)
                                  : !(room_below >= want || room_below >= room_above);
        s32 room = std::min<s32>(H, above ? room_above : room_below);
        s32 lines = std::min<s32>(total, std::min<s32>(max_lines, std::max<s32>(1, (room - fixed) / kGlossLine)));
        if (s_scroll > total - lines) s_scroll = std::max<s32>(0, total - lines);
        const s32 ph = fixed + lines * kGlossLine;

        s32 sx = std::clamp<s32>(w.x0 - 24, 0, 1280 - W);
        s32 sy = above ? anchor_top - 8 - ph : anchor_bottom + 8;
        sy = std::clamp<s32>(sy, 0, 720 - ph);
        /* Layer: popup at its top when it hangs below the word, at its bottom when above. */
        s32 ly = std::clamp<s32>(above ? sy + ph - H : sy, 0, 720 - H);
        if (!move_layer(r, sx, ly)) return;
        const s32 x = sx - layer_x();
        const s32 y = sy - layer_y();
        s_pop_x = sx; s_pop_y = sy; s_pop_w = W; s_pop_h = ph;

        r->drawRect(x, y, W, ph, a(kBg));
        draw_context(r, w, x, y, W);
        r->drawRect(x, y + 32, W, 2, a(kAccent));

        s32 cy = y + 32 + 8 + 28;
        const char *head = e ? e->headword.c_str() : w.surface.c_str();
        s32 hx = x + kPad + text_w(r, head, kHeadFont) + 10;
        r->drawString(head, false, x + kPad, cy, kHeadFont, a(kText));
        if (e && !e->reading.empty()) r->drawString(e->reading.c_str(), false, hx, cy, kReadFont, a(kReading));
        char idx[24];
        snprintf(idx, sizeof(idx), "%d/%zu", e ? s_entry + 1 : 0, s_entries.size());
        r->drawString(idx, false, x + W - kPad - text_w(r, idx, kSmallFont), cy - 8, kSmallFont, a(kDim));
        cy += 6;
        if (s_layout.has_rule) {
            std::string line = e->surface;
            if (!e->rule.empty()) {
                std::string rule = e->rule;
                for (size_t p = 0; (p = rule.find('+', p)) != std::string::npos; p += 3) rule.replace(p, 1, " › ");
                line += "   ← " + rule;
            }
            cy += 20;
            r->drawString(line.c_str(), false, x + kPad, cy, kRuleFont, a(kRule), W - 2 * kPad);
        }
        cy += 8;
        r->drawRect(x + kPad, cy, W - 2 * kPad, 1, a(tsl::Color{ 0x4, 0x4, 0x5, 0xF }));
        cy += 6;
        const s32 gl_top = cy;
        cy += 17;
        for (s32 i = 0; i < lines; ++i) {
            r->drawString(s_layout.gloss[s_scroll + i].c_str(), false, x + kPad, cy, kGlossFont, a(kGloss));
            cy += kGlossLine;
        }
        if (total > lines) { /* scroll bar */
            s32 track = lines * kGlossLine;
            s32 bar = std::max<s32>(10, track * lines / total);
            s32 off = (track - bar) * s_scroll / std::max<s32>(1, total - lines);
            r->drawRect(x + W - 5, gl_top + off, 3, bar, a(kDim));
        }
        /* Footer: controls, and which word of the scan this is. */
        s32 fy = y + ph - 9;
        char hint[160];
        snprintf(hint, sizeof(hint), " entry    word    scroll    rescan    close");
        r->drawString(hint, false, x + kPad, fy, kSmallFont, a(kDim));
        char pos[24];
        snprintf(pos, sizeof(pos), "word %d/%zu", s_sel + 1, s_scan_words.size());
        r->drawString(pos, false, x + W - kPad - text_w(r, pos, kSmallFont), fy, kSmallFont, a(kDim));
    }
};

static void release_scan_state(void) {
    s_line_box_n = 0;
    std::vector<ScanWord>().swap(s_scan_words);
    std::vector<std::vector<uint32_t>>().swap(s_scan_lines);
    std::vector<EntryView>().swap(s_entries);
    s_entries_for = -1;
    s_entry = 0;
    s_sel = -1;
    s_popup = false;
    s_layout = PopupLayout();
    tsl::gfx::Renderer::flushGlyphCache();
}

static void start_scan_after(int frames) {
    if (s_capture_countdown > 0) {
        evlog("scan request (%d frames) ignored: countdown already %d", frames, s_capture_countdown);
        return;
    }
    evlog("scan request: capture in %d frames (popup was %s, word %d)", frames, s_popup ? "open" : "closed", s_sel);
    s_capture_countdown = frames;
    s_popup = false;
    s_sel = -1;
    s_entries.clear();
    s_entries_for = -1;
    s_layout.word = -2;
    set_pill("Scanning…");
}

static void start_scan(void) {
    start_scan_after(kBlankFramesBeforeCapture);
}

class PopupGui : public tsl::Gui {
public:
    virtual tsl::elm::Element* createUI() override {
        return new PopupScreen();
    }

    virtual void update() override {
        /* Reopen detection that does not depend on how the overlay was closed. While it is
         * hidden, libtesla's main thread sleeps in eventWait, so update() stops running; a
         * gap of more than 0.5 s between two updates means it was closed and opened again.
         * onShow() alone was not enough: after closing with the scan combo (libtesla hides
         * from its input thread, mid-frame) while a popup was open, reopening showed the
         * old popup instead of rescanning; closing with B first worked (console 2026-11-05).
         * The tick is taken at the END of update(), so a 400 ms scan inside it never counts. */
        static u64 s_last_update_end = 0;
        const u64 now = armGetSystemTick();
        if (s_hide_count != s_hide_logged) {
            s_hide_logged = s_hide_count;
            evlog("onHide #%u seen by update()", (unsigned)s_hide_logged);
        }
        if (s_last_update_end != 0 && armTicksToNs(now - s_last_update_end) > 500000000ULL) {
            evlog("update gap %llu ms: treated as reopen", (unsigned long long)(armTicksToNs(now - s_last_update_end) / 1000000ULL));
            s_scan_on_show = true;
        }

        if (!s_assets_probed) {
            load_assets_if_needed();
        }
        if (s_scan_on_show) {
            evlog("scan on show: countdown was %d", s_capture_countdown);
            s_scan_on_show = false;
            s_capture_failures = 0;
            s_capture_countdown = 0; /* a countdown left from before the close must not block it */
            start_scan_after(kFramesBeforeFirstCapture);
        }
        if (s_capture_countdown > 0) {
            s_capture_countdown--;
            if (s_capture_countdown == 0) {
                /* The previous frames presented a transparent layer, so the capture
                 * contains only the game. */
                run_scan_now();
                s_entries_for = -1;
                if (!s_scan_words.empty()) select_word(0);
                if (s_capture_failures > 0 && s_capture_failures <= kMaxAutoRetries) {
                    char keep[sizeof(s_pill)];
                    copy_utf8_trunc(keep, sizeof(keep), s_pill);
                    start_scan_after(30 * s_capture_failures); /* 0.5 s, then 1 s */
                    copy_utf8_trunc(s_pill, sizeof(s_pill), keep); /* keep the codes visible */
                }
            }
        }
        s_last_update_end = armGetSystemTick();
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState joyStickPosLeft,
                             HidAnalogStickState joyStickPosRight) override {
        (void)touchPos;
        (void)joyStickPosLeft;
        if (keysDown & (HidNpadButton_B | HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Y))
            evlog("keys %s%s%s%s (popup %s, countdown %d)", (keysDown & HidNpadButton_B) ? "B " : "",
                  (keysDown & HidNpadButton_ZL) ? "ZL " : "", (keysDown & HidNpadButton_ZR) ? "ZR " : "",
                  (keysDown & HidNpadButton_Y) ? "Y " : "", s_popup ? "open" : "closed", s_capture_countdown);
        if (s_capture_countdown > 0) return !(keysDown & HidNpadButton_B); /* B still closes */

        if (keysDown & HidNpadButton_B) {
            if (s_popup) { s_popup = false; return true; }
            return false; /* libtesla hides the overlay: back to the game */
        }
        if (keysDown & HidNpadButton_Y) { s_capture_failures = 0; start_scan(); return true; }
        if (keysDown & HidNpadButton_Plus) {
            /* Unload safir: nx-ovlloader goes back to the Tesla menu. */
            tsl::Overlay::get()->close();
            return true;
        }
        if (keysDown & HidNpadButton_A) {
            if (s_sel < 0) select_word(0);
            else s_popup = !s_popup;
            return true;
        }
        if (keysDown & HidNpadButton_R) { if (s_popup) step_entry(+1); return true; }
        if (keysDown & HidNpadButton_L) { if (s_popup) step_entry(-1); return true; }
        if (keysDown & HidNpadButton_AnyRight && !(keysDown & HidNpadButton_StickRRight)) { select_word(s_sel < 0 ? 0 : s_sel + 1); return true; }
        if (keysDown & HidNpadButton_AnyLeft && !(keysDown & HidNpadButton_StickRLeft))   { select_word(s_sel < 0 ? 0 : s_sel - 1); return true; }
        if (keysDown & HidNpadButton_AnyDown && !(keysDown & HidNpadButton_StickRDown)) { select_line(+1); return true; }
        if (keysDown & HidNpadButton_AnyUp && !(keysDown & HidNpadButton_StickRUp))     { select_line(-1); return true; }

        /* Scroll the glossary: ZL/ZR, or the right stick (repeats while held). */
        static u32 repeat = 0;
        int dir = 0;
        if ((keysDown & HidNpadButton_ZR) || joyStickPosRight.y < -16000) dir = +1;
        if ((keysDown & HidNpadButton_ZL) || joyStickPosRight.y > 16000) dir = -1;
        if (dir != 0 && s_popup) {
            bool stick = (keysDown & (HidNpadButton_ZL | HidNpadButton_ZR)) == 0;
            if (!stick || (repeat++ % 6) == 0) s_scroll = std::max(0, s_scroll + dir);
            return true;
        }
        /* Right stick left/right: entries (repeats while held). */
        static u32 repeat_x = 0;
        int dx = joyStickPosRight.x > 16000 ? +1 : joyStickPosRight.x < -16000 ? -1 : 0;
        if (dx != 0 && s_popup) {
            if ((repeat_x++ % 12) == 0) step_entry(dx);
            return true;
        }
        repeat = 0;
        repeat_x = 0;
        (void)keysHeld;
        return true;
    }
};

class SafirOverlay : public tsl::Overlay {
public:
    virtual void initServices() override {
        /* tsl::loop() already wraps initServices() in an sm session. */
        tsl::cfg::FullScreen = true; /* popup over the game: initScreen() runs after this */
        s_rc_capssc = capsscInitialize();
        s_rc_pminfo = pminfoInitialize();

        fsdevMountSdmc();
        load_scan_combo();
        /* Long-lived buffers before the framebuffers, so they never split the heap later.
         * Glyph cache storage, fixed for the overlay's life: 1024 slots + 160 KB of bitmaps
         * (three font sizes x a few hundred characters on screen at once). */
        static const u32 kGlyphSlots = 1024;
        static const size_t kGlyphArena = 160 * 1024;
        choose_ocr_model(kPageCacheBytes + kGlyphSlots * sizeof(SafirGlyph) + kGlyphArena); /* the biggest one first */
        s_page_cache = malloc(kPageCacheBytes);
        {
            SafirGlyph *slots = (SafirGlyph *)malloc(kGlyphSlots * sizeof(SafirGlyph));
            u8 *arena = (u8 *)malloc(kGlyphArena);
            if (slots && arena) tsl::gfx::Renderer::setGlyphCacheStorage(slots, kGlyphSlots, arena, kGlyphArena);
            else { free(slots); free(arena); } /* the small built-in storage stays */
        }
        choose_popup_size();          /* then the widest popup layer that still fits */

        safir_ledger_init();

        u64 app_pid = 0;
        u64 app_tid = 0;
        if (R_SUCCEEDED(s_rc_pminfo) &&
            R_SUCCEEDED(pmdmntGetApplicationProcessId(&app_pid)) && app_pid != 0) {
            pminfoGetProgramId(&app_tid, app_pid);
        }

        s_switch_cfg.games_ini_path = "sdmc:/config/safir/games.ini";
        s_switch_cfg.title_id = app_tid ? app_tid : 0x0100ABCD12340000ULL;

        safir_ui_init(&s_switch_dict, nullptr, nullptr, &s_switch_cfg);
        safir_bg_worker_init(nullptr, &s_switch_dict);
        {
            /* After both inits: they reset their region to the old default. */
            SafirRect r;
            bool from_ini = safir_games_ini_load(s_switch_cfg.games_ini_path, s_switch_cfg.title_id, &r);
            if (!from_ini) {
                /* Default: scan the whole screen (any text box layout, NVL pages). */
                r = safir_rect_make(0, 0, SAFIR_SCREEN_WIDTH, SAFIR_SCREEN_HEIGHT);
            }
            s_scan_rect = r;
            safir_bg_worker_set_dialogue_rect(r);
            snprintf(s_region_status, sizeof(s_region_status), "Game %016llX: %s %u,%u %ux%u",
                     (unsigned long long)s_switch_cfg.title_id,
                     from_ini ? "region from games.ini" : "full screen", r.x, r.y, r.w, r.h);
        }
        safir_bg_worker_set_enabled(false); /* never auto-start the Core-3 worker */
        evlog("overlay loaded: %s; scan combo %s", s_region_status, s_scan_combo_name);
        evlog("%s", s_ocr_model_note);
    }

    virtual void exitServices() override {
        /* Order matters. The 2168-0002 crash in ovlloader (Data Abort at 0x30 inside
         * _lseek_r <- _fclose_r <- exit) came from unmounting sdmc while the
         * dictionary's FILE* was still open: libc's exit() then flushed that stream
         * through a NULL devoptab entry. Stop the worker, close every file, THEN unmount. */
        safir_bg_worker_stop();
        safir_ui_cleanup();

        safir_engine_lock();
        safir_dict_close(&s_switch_dict);
        s_dict_loaded = false;
        safir_engine_unlock();

        safir_dict_set_page_cache(&s_switch_dict, nullptr, 0);
        free(s_page_cache);
        s_page_cache = nullptr;

        release_capture_buffers();

        fsdevUnmountDevice("sdmc");
        if (R_SUCCEEDED(s_rc_capssc)) capsscExit();
        if (R_SUCCEEDED(s_rc_pminfo)) pminfoExit();
    }

    virtual void onShow() override {
        ::requestForeground(true);  /* tells the Core-3 worker to stand down */
        s_scan_on_show = true;      /* every time the overlay opens: scan what is on screen */
        evlog("onShow (popup %s, word %d, countdown %d, scans so far %u)", s_popup ? "open" : "closed", s_sel,
              s_capture_countdown, (unsigned)s_scan_no);
    }

    virtual void onHide() override {
        ::requestForeground(false);
        s_hide_count = s_hide_count + 1; /* may run on libtesla's input thread: no file I/O here */
    }

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<PopupGui>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<SafirOverlay>(argc, argv);
}
#else
/* Host stub entrypoint */
int main(void) {
    return 0;
}
#endif
