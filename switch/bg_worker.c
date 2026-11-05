#include "bg_worker.h"
#include "mock_libnx.h"
#include "safir_crop.h"
#include "safir_layout.h"
#include "safir_ledger.h"
#include <string.h>

#define BG_THREAD_STACK_SIZE 0x10000

#if defined(_MSC_VER)
__declspec(align(0x1000)) static uint8_t g_bg_stack[BG_THREAD_STACK_SIZE];
#else
static uint8_t g_bg_stack[BG_THREAD_STACK_SIZE] __attribute__((aligned(0x1000)));
#endif

atomic_bool g_bg_ocr_enabled = true;
static atomic_bool g_bg_worker_running = false;

static Thread g_bg_thread;
static Mutex g_bg_mutex;
static CondVar g_bg_cond;
static Mutex g_cache_mutex;
/* Serializes everything that touches shared engine state: POOL_OCR_SCRATCH (both threads
 * reset it), the OCR backend's dHash cache, and the dictionary's FILE* + page caches. */
static Mutex g_engine_mutex;

void safir_engine_lock(void) {
    mutexLock(&g_engine_mutex);
}

void safir_engine_unlock(void) {
    mutexUnlock(&g_engine_mutex);
}

/* Full-frame crops (640x360 = 230,400 B at the 2x scale) do not fit POOL_OCR_SCRATCH next
 * to the recognizer's arenas, so main_ovl.cpp (the heap owner) registers one buffer for them. */
static uint8_t *s_frame_crop_buf = NULL;
static size_t s_frame_crop_size = 0;

void safir_set_frame_crop_buffer(uint8_t *buf, size_t size) {
    s_frame_crop_buf = buf;
    s_frame_crop_size = size;
}

uint8_t *safir_acquire_crop(size_t bytes) {
    /* Caller holds the engine lock. Prefer the registered buffer so scratch stays free
     * for the line finder (~41 KB at full frame) and the network arena (53,856 B). */
    if (s_frame_crop_buf && bytes <= s_frame_crop_size) return s_frame_crop_buf;
    safir_pool_reset(POOL_OCR_SCRATCH);
    return (uint8_t *)safir_alloc(POOL_OCR_SCRATCH, bytes);
}

static SafirOcrResult s_cached_result;
static uint64_t s_cached_dhash = 0;
static bool s_has_cache = false;

static SafirRect s_dialogue_rect;
static SafirOcrBackend *s_ocr_backend = NULL;
static SafirDict *s_dict = NULL;
static PadState s_pad;

/* Dialogue bounds */
#define MAX_CROP_W 1280
#define MAX_CROP_H 720 /* full-screen scans are the default */

bool safir_bg_worker_init(SafirOcrBackend *backend, SafirDict *dict) {
    s_ocr_backend = backend;
    s_dict = dict;
    s_dialogue_rect = safir_default_dialogue_rect();

    mutexInit(&g_bg_mutex);
    condvarInit(&g_bg_cond);
    mutexInit(&g_cache_mutex);

    /* g_engine_mutex is deliberately NOT (re)initialized here: init may run more than
     * once, and re-initializing a held mutex deadlocks. A zeroed static libnx Mutex is
     * already valid; the host mock initializes on first lock. */

    padInitializeDefault(&s_pad);
    hidInitializeTouchScreen();
    /* caps:sc is initialized exactly once by the overlay's initServices(); a second
     * capsscInitialize() here leaked a service reference that capsscExit() never dropped. */

    s_has_cache = false;
    s_cached_dhash = 0;
    memset(&s_cached_result, 0, sizeof(s_cached_result));

    atomic_store(&g_bg_ocr_enabled, true);
    atomic_store(&g_bg_worker_running, false);

    return true;
}

bool safir_bg_worker_start(void) {
    if (atomic_load(&g_bg_worker_running)) {
        return true; /* Already running */
    }

    atomic_store(&g_bg_worker_running, true);

    /* Pin background thread explicitly to CPU Core 3 at low priority 0x3B */
    Result rc = threadCreate(&g_bg_thread, bg_ocr_loop, NULL, g_bg_stack, BG_THREAD_STACK_SIZE, 0x3B, 3);
    if (R_FAILED(rc)) {
        atomic_store(&g_bg_worker_running, false);
        return false;
    }

    rc = threadStart(&g_bg_thread);
    if (R_FAILED(rc)) {
        atomic_store(&g_bg_worker_running, false);
        return false;
    }

    return true;
}

void safir_bg_worker_stop(void) {
    if (!atomic_load(&g_bg_worker_running)) {
        return;
    }

    atomic_store(&g_bg_worker_running, false);

    /* Wake condvar so thread exits wait loop immediately */
    condvarWakeAll(&g_bg_cond);

    /* Perform clean thread join to prevent core stalls */
    threadWaitForExit(&g_bg_thread);
    threadClose(&g_bg_thread);
}

bool safir_bg_worker_is_running(void) {
    return atomic_load(&g_bg_worker_running);
}

void safir_bg_worker_set_enabled(bool enabled) {
    atomic_store(&g_bg_ocr_enabled, enabled);
    if (enabled) {
        if (!atomic_load(&g_bg_worker_running)) {
            safir_bg_worker_start();
        }
        condvarWakeOne(&g_bg_cond);
    }
}

bool safir_bg_worker_is_enabled(void) {
    return atomic_load(&g_bg_ocr_enabled);
}

void safir_bg_worker_set_dialogue_rect(SafirRect rect) {
    mutexLock(&g_cache_mutex);
    s_dialogue_rect = rect;
    s_has_cache = false; /* Invalidate cache on region change */
    mutexUnlock(&g_cache_mutex);
}

SafirRect safir_bg_worker_get_dialogue_rect(void) {
    mutexLock(&g_cache_mutex);
    SafirRect r = s_dialogue_rect;
    mutexUnlock(&g_cache_mutex);
    return r;
}

void safir_bg_worker_set_backend(SafirOcrBackend *backend) {
    mutexLock(&g_cache_mutex);
    s_ocr_backend = backend;
    s_has_cache = false;
    mutexUnlock(&g_cache_mutex);
}

bool safir_bg_worker_get_cached_result(SafirOcrResult *out_result, uint64_t *out_dhash) {
    if (!out_result) return false;

    mutexLock(&g_cache_mutex);
    if (!s_has_cache) {
        mutexUnlock(&g_cache_mutex);
        return false;
    }

    *out_result = s_cached_result;
    if (out_dhash) {
        *out_dhash = s_cached_dhash;
    }
    mutexUnlock(&g_cache_mutex);
    return true;
}

void safir_bg_worker_invalidate_cache(void) {
    mutexLock(&g_cache_mutex);
    s_has_cache = false;
    s_cached_dhash = 0;
    memset(&s_cached_result, 0, sizeof(s_cached_result));
    mutexUnlock(&g_cache_mutex);
}

static bool bg_worker_step_locked_part(SafirRect rect, SafirOcrBackend *backend, SafirDict *dict);

bool safir_bg_worker_step_once(void) {
    /* Poll game text-advance buttons */
    padUpdate(&s_pad);
    u64 down = padGetButtonsDown(&s_pad);

    HidTouchScreenState touch;
    memset(&touch, 0, sizeof(touch));
    hidGetTouchScreenStates(&touch, 1);

    bool advance_pressed = (down & (HidNpadButton_A | HidNpadButton_ZR)) != 0;
    bool screen_touched = (touch.count > 0);

    if (!advance_pressed && !screen_touched) {
        svcSleepThread(16000000ULL); /* ~16ms sleep (1 frame) */
        return false;
    }

    /* Wait 280ms after advance input for VN typewriter text animation to settle */
    svcSleepThread(280000000ULL);

    /* Abort if worker was disabled or overlay entered foreground during settle */
    if (!atomic_load(&g_bg_ocr_enabled) || isForeground()) {
        return false;
    }

    mutexLock(&g_cache_mutex);
    SafirRect rect = s_dialogue_rect;
    SafirOcrBackend *backend = s_ocr_backend;
    SafirDict *dict = s_dict;
    mutexUnlock(&g_cache_mutex);

    if (rect.w == 0 || rect.h == 0 || rect.w > MAX_CROP_W || rect.h > MAX_CROP_H) {
        return false;
    }

    safir_engine_lock();
    bool ok = bg_worker_step_locked_part(rect, backend, dict);
    safir_engine_unlock();
    return ok;
}

static bool bg_worker_step_locked_part(SafirRect rect, SafirOcrBackend *backend, SafirDict *dict) {
    uint16_t crop_w = (rect.w > 0) ? ((rect.w + 1) / 2) : 550;
    uint16_t crop_h = (rect.h > 0) ? ((rect.h + 1) / 2) : 105;
    size_t num_pixels = (size_t)crop_w * crop_h;
    safir_pool_reset(POOL_OCR_SCRATCH);
    uint8_t *crop_gray = safir_acquire_crop(num_pixels);
    if (!crop_gray) {
        return false;
    }

    Result rc = safir_capture_crop_grayscale(crop_gray, rect, 100000000ULL);
    if (R_FAILED(rc)) {
        return false;
    }

    /* Auto-invert polarity if light text on dark background */
    safir_crop_auto_invert(crop_gray, crop_w, crop_h);

    /* Compute 64-bit perceptual hash (dHash) */
    uint64_t dhash = safir_compute_dhash(crop_gray, crop_w, crop_h);

    /* Check if screen line changed */
    bool line_changed = false;
    mutexLock(&g_cache_mutex);
    if (!s_has_cache || !safir_dhash_is_similar(dhash, s_cached_dhash)) {
        line_changed = true;
    }
    mutexUnlock(&g_cache_mutex);

    if (!line_changed) {
        return false;
    }

    if (!backend || !backend->recognize) {
        return false;
    }

    /* Run OCR backend and pre-calculate word boundaries */
    SafirOcrResult result;
    memset(&result, 0, sizeof(result));

    bool ok = backend->recognize(backend, crop_gray, crop_w, crop_h, rect, &result);
    if (!ok) {
        return false;
    }

    result.dhash = dhash;
    result.cached = false;

    /* Pre-calculate word boundaries in background */
    if (dict) {
        for (int l = 0; l < (int)result.line_count; ++l) {
            SafirLineTokens tokens;
            safir_dict_tokenize_line(dict, &result.lines[l], &tokens);
        }
    }

    /* Update cache */
    mutexLock(&g_cache_mutex);
    s_cached_result = result;
    s_cached_dhash = dhash;
    s_has_cache = true;
    mutexUnlock(&g_cache_mutex);

    return true;
}

void bg_ocr_loop(void *arg) {
    (void)arg;

    while (atomic_load(&g_bg_worker_running)) {
        /* Architecture A / Rule 3: Easy Kill-Switch CondVar Wait */
        mutexLock(&g_bg_mutex);
        while (!atomic_load(&g_bg_ocr_enabled) && atomic_load(&g_bg_worker_running)) {
            condvarWait(&g_bg_cond, &g_bg_mutex);
        }
        mutexUnlock(&g_bg_mutex);

        if (!atomic_load(&g_bg_worker_running)) {
            break;
        }

        /* If overlay UI is in foreground, yield CPU and skip capture */
        if (isForeground()) {
            svcSleepThread(100000000ULL); /* 100ms */
            continue;
        }

        /* Run event-driven check */
        safir_bg_worker_step_once();

        /* Always yield Core 3 at least 100ms per iteration to prevent UI starvation */
        svcSleepThread(100000000ULL);
    }
}
