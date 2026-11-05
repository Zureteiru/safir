#ifndef SAFIR_BG_WORKER_H
#define SAFIR_BG_WORKER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "safir_types.h"
#include "safir_ocr.h"
#include "safir_dict.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Portable C11 / C++ atomic boolean definition compatible with MSVC, Clang, GCC, and Switch libnx
 */
typedef volatile long safir_atomic_bool;
#ifndef atomic_bool
#define atomic_bool safir_atomic_bool
#endif

static inline bool safir_atomic_load_bool(const volatile long *p) {
#if defined(_MSC_VER)
    return (*p != 0);
#else
    return __atomic_load_n(p, __ATOMIC_SEQ_CST) != 0;
#endif
}

static inline void safir_atomic_store_bool(volatile long *p, bool val) {
#if defined(_MSC_VER)
    InterlockedExchange(p, val ? 1 : 0);
#else
    long v = val ? 1 : 0;
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
#endif
}

#ifndef atomic_load
#define atomic_load(p) safir_atomic_load_bool(p)
#endif
#ifndef atomic_store
#define atomic_store(p, val) safir_atomic_store_bool(p, val)
#endif

/**
 * Architecture A & Rule 3 Easy Kill-Switch:
 * Atomic toggle controlling the Core-3 background pre-OCR thread.
 * When false: Core-3 thread sleeps on CondVar (0% CPU, 0 captures).
 * When true: Event-driven OCR triggers after text advance inputs.
 */
extern atomic_bool g_bg_ocr_enabled;

/**
 * Initializes the background worker subsystem, synchronization mutexes, and condvar.
 */
bool safir_bg_worker_init(SafirOcrBackend *backend, SafirDict *dict);

/**
 * Spawns the background Core-3 worker thread using:
 * svcCreateThread(&g_bg_thread, bg_ocr_loop, NULL, g_bg_stack, 0x10000, 0x2C, 3)
 */
bool safir_bg_worker_start(void);

/**
 * Gracefully signals the worker to terminate, wakes condvar, and joins thread cleanly.
 */
void safir_bg_worker_stop(void);

/**
 * Returns true if the background worker thread is currently running.
 */
bool safir_bg_worker_is_running(void);

/**
 * Toggles the background pre-OCR kill switch (ON / OFF).
 * Wakes the condvar when toggled ON.
 */
void safir_bg_worker_set_enabled(bool enabled);

/**
 * Returns whether background pre-OCR is currently enabled.
 */
bool safir_bg_worker_is_enabled(void);

/**
 * Sets the active game dialogue rectangle to monitor and crop.
 */
void safir_bg_worker_set_dialogue_rect(SafirRect rect);

/**
 * Gets the current active game dialogue rectangle.
 */
SafirRect safir_bg_worker_get_dialogue_rect(void);

/**
 * Sets the active OCR backend (e.g. Local VOCR6 vs Google Vision Fallback).
 */
void safir_bg_worker_set_backend(SafirOcrBackend *backend);

/**
 * Thread-safe retrieval of the cached background OCR result.
 * Returns true if a cached result was retrieved and written to out_result.
 */
bool safir_bg_worker_get_cached_result(SafirOcrResult *out_result, uint64_t *out_dhash);

/**
 * Invalidate the background OCR cache (e.g. upon scene change or manual reload).
 */
void safir_bg_worker_invalidate_cache(void);

/**
 * Performs a single step of the event-driven pre-OCR check.
 * Called by bg_ocr_loop or directly in test harnesses.
 * Returns true if an advance was detected and OCR was executed.
 */
bool safir_bg_worker_step_once(void);

/**
 * Background OCR thread entry function passed to svcCreateThread.
 */
void bg_ocr_loop(void *arg);

/**
 * Engine lock: hold it around any use of POOL_OCR_SCRATCH, the OCR backend or the
 * dictionary, from any thread (UI thread and Core-3 worker share all three).
 */
void safir_engine_lock(void);
void safir_engine_unlock(void);

/**
 * Crop buffer for capture regions too large for POOL_OCR_SCRATCH (a full 1280x720
 * frame is 230,400 B at the 2x scale). Registered by main_ovl.cpp, which owns the heap.
 */
void safir_set_frame_crop_buffer(uint8_t *buf, size_t size);

/**
 * Returns a buffer for a 2x grey crop of `bytes` bytes: the registered frame buffer if
 * it is large enough, else POOL_OCR_SCRATCH (reset first). Hold the engine lock.
 */
uint8_t *safir_acquire_crop(size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* SAFIR_BG_WORKER_H */
