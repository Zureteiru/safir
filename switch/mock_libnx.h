#ifndef SWITCH_MOCK_LIBNX_H
#define SWITCH_MOCK_LIBNX_H

#include "safir_types.h"

#if defined(__SWITCH__)
#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

bool isForeground(void);
void requestForeground(bool active);
Result safir_switch_capture_crop(void *out_buf, size_t out_buf_size, u64 timeout_ns);
Result safir_capture_crop_grayscale(uint8_t *out_gray, SafirRect rect, u64 timeout_ns);
/* Per-attempt result codes of the last capture, e.g. "Def=2206-0820 Shot=... ok:App". */
const char *safir_capture_last_diag(void);
/* Buffer for caps:sc JPEG screenshots (official software uses 0x80000 bytes). */
void safir_capture_set_jpeg_buffer(void *buf, size_t size);
/* The JPEG of the last successful JPEG capture (valid until the next capture). */
bool safir_capture_last_jpeg(const uint8_t **data, size_t *size);
#define capsscCaptureRawImageWithTimeout safir_switch_capture_crop

#ifdef __cplusplus
}
#endif

#else

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Standard libnx primitive typedefs */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef u32 Result;
typedef u32 Handle;

#define R_SUCCEEDED(res)   ((res) == 0)
#define R_FAILED(res)      ((res) != 0)

/* Controller Button Bitmasks */
#define HidNpadButton_A           (1ULL << 0)
#define HidNpadButton_B           (1ULL << 1)
#define HidNpadButton_X           (1ULL << 2)
#define HidNpadButton_Y           (1ULL << 3)
#define HidNpadButton_StickL      (1ULL << 4)
#define HidNpadButton_StickR      (1ULL << 5)
#define HidNpadButton_L           (1ULL << 6)
#define HidNpadButton_R           (1ULL << 7)
#define HidNpadButton_ZL          (1ULL << 8)
#define HidNpadButton_ZR          (1ULL << 9)
#define HidNpadButton_Plus        (1ULL << 10)
#define HidNpadButton_Minus       (1ULL << 11)
#define HidNpadButton_Left        (1ULL << 12)
#define HidNpadButton_Up          (1ULL << 13)
#define HidNpadButton_Right       (1ULL << 14)
#define HidNpadButton_Down        (1ULL << 15)
#define HidNpadButton_StickLLeft  (1ULL << 16)
#define HidNpadButton_StickLUp    (1ULL << 17)
#define HidNpadButton_StickLRight (1ULL << 18)
#define HidNpadButton_StickLDown  (1ULL << 19)
#define HidNpadButton_StickRLeft  (1ULL << 20)
#define HidNpadButton_StickRUp    (1ULL << 21)
#define HidNpadButton_StickRRight (1ULL << 22)
#define HidNpadButton_StickRDown  (1ULL << 23)

/* Touch Screen State */
typedef struct HidTouchState {
    u64 delta_time;
    u32 attributes;
    u32 finger_id;
    u32 x;
    u32 y;
    u32 diameter_x;
    u32 diameter_y;
    u32 rotation_angle;
} HidTouchState;

typedef struct HidTouchScreenState {
    s32 count;
    HidTouchState touches[16];
} HidTouchScreenState;

/* Pad State */
typedef struct PadState {
    u64 buttons_cur;
    u64 buttons_old;
    s32 stick_lx;
    s32 stick_ly;
    s32 stick_rx;
    s32 stick_ry;
} PadState;

/* Synchronization Primitives */
typedef struct Mutex {
#if defined(_WIN32)
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t mtx;
#endif
    int initialized;
} Mutex;

typedef struct CondVar {
#if defined(_WIN32)
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t cond;
#endif
    int initialized;
} CondVar;

/* Thread Primitive */
typedef struct Thread {
#if defined(_WIN32)
    HANDLE handle;
    DWORD thread_id;
#else
    pthread_t thread;
#endif
    void (*entry)(void*);
    void *arg;
    void *stack;
    size_t stack_size;
    int priority;
    int cpuid;
    bool active;
    bool joined;
} Thread;

/* Mutex Functions */
static inline void mutexInit(Mutex *m) {
    if (!m) return;
#if defined(_WIN32)
    InitializeCriticalSection(&m->cs);
#else
    pthread_mutex_init(&m->mtx, NULL);
#endif
    m->initialized = 1;
}

static inline void mutexLock(Mutex *m) {
    if (!m) return;
    if (!m->initialized) mutexInit(m);
#if defined(_WIN32)
    EnterCriticalSection(&m->cs);
#else
    pthread_mutex_lock(&m->mtx);
#endif
}

static inline void mutexUnlock(Mutex *m) {
    if (!m || !m->initialized) return;
#if defined(_WIN32)
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->mtx);
#endif
}

/* CondVar Functions */
static inline void condvarInit(CondVar *c) {
    if (!c) return;
#if defined(_WIN32)
    InitializeConditionVariable(&c->cv);
#else
    pthread_cond_init(&c->cond, NULL);
#endif
    c->initialized = 1;
}

static inline void condvarWait(CondVar *c, Mutex *m) {
    if (!c || !m) return;
    if (!c->initialized) condvarInit(c);
#if defined(_WIN32)
    SleepConditionVariableCS(&c->cv, &m->cs, INFINITE);
#else
    pthread_cond_wait(&c->cond, &m->mtx);
#endif
}

static inline void condvarWaitTimeout(CondVar *c, Mutex *m, u64 timeout_ns) {
    if (!c || !m) return;
    if (!c->initialized) condvarInit(c);
#if defined(_WIN32)
    DWORD ms = (DWORD)(timeout_ns / 1000000ULL);
    if (ms == 0 && timeout_ns > 0) ms = 1;
    SleepConditionVariableCS(&c->cv, &m->cs, ms);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(timeout_ns / 1000000000ULL);
    ts.tv_nsec += (long)(timeout_ns % 1000000000ULL);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&c->cond, &m->mtx, &ts);
#endif
}

static inline void condvarWakeOne(CondVar *c) {
    if (!c || !c->initialized) return;
#if defined(_WIN32)
    WakeConditionVariable(&c->cv);
#else
    pthread_cond_signal(&c->cond);
#endif
}

static inline void condvarWakeAll(CondVar *c) {
    if (!c || !c->initialized) return;
#if defined(_WIN32)
    WakeAllConditionVariable(&c->cv);
#else
    pthread_cond_broadcast(&c->cond);
#endif
}

/* Mock State Tracking */
extern bool g_mock_fast_time;
extern u64 g_mock_buttons_held;
extern u64 g_mock_buttons_down;

/* Sleep Function */
static inline void svcSleepThread(s64 nanoseconds) {
    if (g_mock_fast_time || nanoseconds <= 0) {
        /* Fast time still has to yield for real: a worker that never sleeps re-takes
         * the engine lock in a tight loop and starves other threads on host
         * (glibc mutexes are not fair). On the console it always sleeps >= 100 ms. */
#if defined(_WIN32)
        Sleep(0);
#else
        struct timespec ts_y = {0, 20000};
        nanosleep(&ts_y, NULL);
#endif
        return;
    }
#if defined(_WIN32)
    DWORD ms = (DWORD)(nanoseconds / 1000000LL);
    if (ms == 0) ms = 1;
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(nanoseconds / 1000000000LL);
    ts.tv_nsec = (long)(nanoseconds % 1000000000LL);
    nanosleep(&ts, NULL);
#endif
}
extern HidTouchScreenState g_mock_touch;
extern u64 g_mock_title_id;
extern bool g_mock_foreground;
extern u64 g_mock_capture_count;
extern int g_mock_last_thread_cpuid;
extern uint8_t g_mock_screen_frame[1280 * 720 * 4];

/* Thread Functions */
#if defined(_WIN32)
static inline DWORD WINAPI mock_thread_trampoline(LPVOID lpParam) {
    Thread *t = (Thread*)lpParam;
    if (t && t->entry) {
        t->entry(t->arg);
    }
    return 0;
}
#else
static inline void *mock_pthread_trampoline(void *lpParam) {
    Thread *t = (Thread*)lpParam;
    if (t && t->entry) {
        t->entry(t->arg);
    }
    return NULL;
}
#endif

static inline Result svcCreateThread(Thread *thread, void (*entry)(void*), void *arg, void *stack, size_t stack_size, int priority, int cpuid) {
    if (!thread || !entry) return 1;
    memset(thread, 0, sizeof(Thread));
    thread->entry = entry;
    thread->arg = arg;
    thread->stack = stack;
    thread->stack_size = stack_size;
    thread->priority = priority;
    thread->cpuid = cpuid;
    thread->active = false;
    thread->joined = false;
    g_mock_last_thread_cpuid = cpuid;
    return 0;
}

static inline Result svcStartThread(Thread *thread) {
    if (!thread || !thread->entry) return 1;
    thread->active = true;
#if defined(_WIN32)
    thread->handle = CreateThread(NULL, thread->stack_size, mock_thread_trampoline, thread, 0, &thread->thread_id);
    return (thread->handle != NULL) ? 0 : 2;
#else
    int res = pthread_create(&thread->thread, NULL, mock_pthread_trampoline, thread);
    return (res == 0) ? 0 : 2;
#endif
}

static inline Result threadCreate(Thread *thread, void (*entry)(void*), void *arg, void *stack, size_t stack_size, int priority, int cpuid) {
    return svcCreateThread(thread, entry, arg, stack, stack_size, priority, cpuid);
}

static inline Result threadStart(Thread *thread) {
    return svcStartThread(thread);
}

static inline Result threadWaitForExit(Thread *thread) {
    if (!thread) return 1;
    if (thread->active && !thread->joined) {
#if defined(_WIN32)
        if (thread->handle) {
            WaitForSingleObject(thread->handle, INFINITE);
        }
#else
        pthread_join(thread->thread, NULL);
#endif
        thread->joined = true;
        thread->active = false;
    }
    return 0;
}

static inline Result threadClose(Thread *thread) {
    if (!thread) return 1;
    threadWaitForExit(thread);
#if defined(_WIN32)
    if (thread->handle) {
        CloseHandle(thread->handle);
        thread->handle = NULL;
    }
#endif
    return 0;
}

static inline Result svcWaitSynchronization(Handle *handle, Handle count, s64 timeout) {
    (void)handle;
    (void)count;
    (void)timeout;
    return 0;
}

static inline Result svcCloseHandle(Handle handle) {
    (void)handle;
    return 0;
}

/* Pad Functions */
static inline void padInitializeDefault(PadState *pad) {
    if (!pad) return;
    memset(pad, 0, sizeof(PadState));
}

static inline void padUpdate(PadState *pad) {
    if (!pad) return;
    pad->buttons_old = pad->buttons_cur;
    pad->buttons_cur = g_mock_buttons_held;
}

static inline u64 padGetButtons(PadState *pad) {
    if (!pad) return g_mock_buttons_held;
    return pad->buttons_cur;
}

static inline u64 padGetButtonsDown(PadState *pad) {
    (void)pad;
    u64 down = g_mock_buttons_down;
    g_mock_buttons_down = 0; /* Clear single-shot down event */
    return down;
}

/* Touch Screen Functions */
static inline void hidInitializeTouchScreen(void) {
}

static inline s32 hidGetTouchScreenStates(HidTouchScreenState *states, s32 count) {
    if (!states || count <= 0) return 0;
    states[0] = g_mock_touch;
    return 1;
}

/* Frame Capture Functions */
static inline Result capsscInitialize(void) {
    return 0;
}

static inline void capsscExit(void) {
}

SafirRect safir_bg_worker_get_dialogue_rect(void);

static inline Result capsscCaptureRawImageWithTimeout(void *out_buf, size_t out_buf_size, u64 timeout_ns) {
    (void)timeout_ns;
    if (!out_buf || out_buf_size == 0) return 1;
    if (out_buf_size < sizeof(g_mock_screen_frame)) {
        SafirRect rect = safir_bg_worker_get_dialogue_rect();
        if (rect.w > 0 && rect.h > 0 && (uint32_t)rect.x + rect.w <= 1280 && (uint32_t)rect.y + rect.h <= 720 && out_buf_size >= (size_t)rect.w * rect.h * 4) {
            uint8_t *dst = (uint8_t*)out_buf;
            for (uint16_t y = 0; y < rect.h; ++y) {
                const uint8_t *src = g_mock_screen_frame + ((size_t)(rect.y + y) * 1280 + rect.x) * 4;
                memcpy(dst + ((size_t)y * rect.w) * 4, src, (size_t)rect.w * 4);
            }
        } else {
            size_t copy_bytes = out_buf_size;
            memcpy(out_buf, g_mock_screen_frame, copy_bytes);
        }
    } else {
        memcpy(out_buf, g_mock_screen_frame, sizeof(g_mock_screen_frame));
    }
    g_mock_capture_count++;
    return 0;
}

static inline Result safir_capture_crop_grayscale(uint8_t *out_gray, SafirRect rect, u64 timeout_ns) {
    (void)timeout_ns;
    if (!out_gray || rect.w == 0 || rect.h == 0) return 1;
    if ((uint32_t)rect.x + rect.w > 1280 || (uint32_t)rect.y + rect.h > 720) return 2;

    uint16_t dw = (rect.w + 1) / 2;
    uint16_t dh = (rect.h + 1) / 2;

    for (uint16_t y = 0; y < dh; ++y) {
        const uint8_t *src_row = g_mock_screen_frame + ((size_t)(rect.y + y * 2) * 1280 + rect.x) * 4;
        uint8_t *dst_row = out_gray + (size_t)y * dw;
        for (uint16_t x = 0; x < dw; ++x) {
            uint8_t r = src_row[x * 2 * 4 + 0];
            uint8_t g = src_row[x * 2 * 4 + 1];
            uint8_t b = src_row[x * 2 * 4 + 2];
            dst_row[x] = (uint8_t)((77 * (uint32_t)r + 150 * (uint32_t)g + 29 * (uint32_t)b) >> 8);
        }
    }
    g_mock_capture_count++;
    return 0;
}

/* Filesystem dev stubs */
static inline Result fsdevMountSdmc(void) { return 0; }
static inline Result fsdevUnmountDevice(const char *name) { (void)name; return 0; }

/* PM / Title Functions */
static inline Result pmdmntGetApplicationProcessId(u64 *pid) {
    if (pid) *pid = 0x1234;
    return 0;
}

static inline Result pminfoGetProgramId(u64 *program_id, u64 pid) {
    (void)pid;
    if (program_id) *program_id = g_mock_title_id;
    return 0;
}

/* Overlay Foreground Control */
static inline void requestForeground(bool active) {
    g_mock_foreground = active;
}

static inline bool isForeground(void) {
    return g_mock_foreground;
}

/* Mock Helpers for Unit Tests */
void mock_libnx_reset(void);
void mock_libnx_set_buttons_down(u64 buttons);
void mock_libnx_set_buttons_held(u64 buttons);
void mock_libnx_set_touch(s32 count, u32 x, u32 y);
void mock_libnx_set_screen_frame(const uint8_t *rgba_1280x720);
void mock_libnx_set_title_id(u64 title_id);
u64 mock_libnx_get_capture_count(void);
void mock_libnx_reset_capture_count(void);
int mock_libnx_get_last_thread_cpuid(void);

#ifdef __cplusplus
}
#endif

#endif /* !defined(__SWITCH__) */

#endif /* SWITCH_MOCK_LIBNX_H */
