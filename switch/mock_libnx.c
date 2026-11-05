#include "mock_libnx.h"
#include "safir_crop.h"
#include <stdio.h>

#if !defined(__SWITCH__)

bool g_mock_fast_time = true;
u64 g_mock_buttons_held = 0;
u64 g_mock_buttons_down = 0;
HidTouchScreenState g_mock_touch;
u64 g_mock_title_id = 0x0100ABCD12340000ULL;
bool g_mock_foreground = false;
u64 g_mock_capture_count = 0;
int g_mock_last_thread_cpuid = -1;
uint8_t g_mock_screen_frame[1280 * 720 * 4];

void mock_libnx_reset(void) {
    g_mock_buttons_held = 0;
    g_mock_buttons_down = 0;
    memset(&g_mock_touch, 0, sizeof(g_mock_touch));
    g_mock_title_id = 0x0100ABCD12340000ULL;
    g_mock_foreground = false;
    g_mock_capture_count = 0;
    g_mock_last_thread_cpuid = -1;
    memset(g_mock_screen_frame, 0, sizeof(g_mock_screen_frame));
}

void mock_libnx_set_buttons_down(u64 buttons) {
    g_mock_buttons_down = buttons;
}

void mock_libnx_set_buttons_held(u64 buttons) {
    g_mock_buttons_held = buttons;
}

void mock_libnx_set_touch(s32 count, u32 x, u32 y) {
    g_mock_touch.count = count;
    if (count > 0) {
        g_mock_touch.touches[0].x = x;
        g_mock_touch.touches[0].y = y;
        g_mock_touch.touches[0].finger_id = 1;
    }
}

void mock_libnx_set_screen_frame(const uint8_t *rgba_1280x720) {
    if (rgba_1280x720) {
        memcpy(g_mock_screen_frame, rgba_1280x720, sizeof(g_mock_screen_frame));
    } else {
        memset(g_mock_screen_frame, 0, sizeof(g_mock_screen_frame));
    }
}

void mock_libnx_set_title_id(u64 title_id) {
    g_mock_title_id = title_id;
}

u64 mock_libnx_get_capture_count(void) {
    return g_mock_capture_count;
}

void mock_libnx_reset_capture_count(void) {
    g_mock_capture_count = 0;
}

int mock_libnx_get_last_thread_cpuid(void) {
    return g_mock_last_thread_cpuid;
}

#else

#include "mock_libnx.h"

static bool s_switch_foreground = false;

bool isForeground(void) {
    return s_switch_foreground;
}

void requestForeground(bool active) {
    s_switch_foreground = active;
}

Result safir_switch_capture_crop(void *out_buf, size_t out_buf_size, u64 timeout_ns) {
    (void)out_buf;
    (void)out_buf_size;
    (void)timeout_ns;
    u64 out_size = 0, out_w = 0, out_h = 0;
    Result rc = capsscOpenRawScreenShotReadStream(&out_size, &out_w, &out_h, ViLayerStack_Default, (s64)timeout_ns);
    if (R_SUCCEEDED(rc)) {
        u64 bytes_read = 0;
        capsscReadRawScreenShotReadStream(&bytes_read, out_buf, out_buf_size, 0);
        capsscCloseRawScreenShotReadStream();
        return 0;
    }
    return rc;
}

/* One capture attempt on one layer stack. */
static Result capture_once(uint8_t *out_gray, SafirRect rect, ViLayerStack stack, u64 timeout_ns) {
    u64 out_size = 0, out_w = 0, out_h = 0;
    Result rc = capsscOpenRawScreenShotReadStream(&out_size, &out_w, &out_h, stack, (s64)timeout_ns);
    if (R_FAILED(rc)) {
        return rc;
    }
    if (out_w != 1280 || out_h != 720 || out_size < 1280ULL * 720ULL * 4ULL) {
        capsscCloseRawScreenShotReadStream();
        return MAKERESULT(0x1FF, 3); /* unexpected raw screenshot geometry */
    }

    uint16_t dw = (uint16_t)((rect.w + 1) / 2);
    uint16_t dh = (uint16_t)((rect.h + 1) / 2);

    static uint8_t row_rgba[1280 * 4]; /* static: keeps 5 KB off the overlay's small stack */
    for (uint16_t y = 0; y < dh; ++y) {
        u32 src_y = (u32)rect.y + (u32)y * 2u;
        if (src_y >= 720) src_y = 719;
        size_t offset = ((size_t)src_y * 1280 + rect.x) * 4;
        size_t row_bytes = (size_t)rect.w * 4;
        u64 bytes_read = 0;
        rc = capsscReadRawScreenShotReadStream(&bytes_read, row_rgba, row_bytes, offset);
        if (R_FAILED(rc) || bytes_read < row_bytes) {
            capsscCloseRawScreenShotReadStream();
            return R_FAILED(rc) ? rc : MAKERESULT(0x1FF, 4);
        }

        uint8_t *dst_row = out_gray + (size_t)y * dw;
        for (uint16_t x = 0; x < dw; ++x) {
            size_t sx = (size_t)x * 2u;
            if (sx >= rect.w) sx = (size_t)rect.w - 1;
            uint8_t r = row_rgba[sx * 4 + 0];
            uint8_t g = row_rgba[sx * 4 + 1];
            uint8_t b = row_rgba[sx * 4 + 2];
            dst_row[x] = (uint8_t)((77 * (uint32_t)r + 150 * (uint32_t)g + 29 * (uint32_t)b) >> 8);
        }
    }
    capsscCloseRawScreenShotReadStream();
    return 0;
}

/* caps:sc JPEG screenshot (10.0.0+, no debug mode needed), then region-only decode.
 * The JPEG buffer is owned by main_ovl.cpp (the only file allowed to use the heap,
 * see tools/check_memory_ledger.py) and registered with safir_capture_set_jpeg_buffer(). */
static uint8_t *s_jpeg_buf = NULL;
static size_t s_jpeg_buf_size = 0;
static size_t s_jpeg_last_size = 0; /* bytes of the last successful JPEG capture */

bool safir_capture_last_jpeg(const uint8_t **data, size_t *size) {
    if (!s_jpeg_buf || s_jpeg_last_size == 0) return false;
    *data = s_jpeg_buf;
    *size = s_jpeg_last_size;
    return true;
}

void safir_capture_set_jpeg_buffer(void *buf, size_t size) {
    s_jpeg_buf = (uint8_t *)buf;
    s_jpeg_buf_size = size;
}

static Result capture_jpeg(uint8_t *out_gray, SafirRect rect, ViLayerStack stack, u64 timeout_ns) {
    if (hosversionBefore(10, 0, 0)) return MAKERESULT(0x1FF, 12);
    if (!s_jpeg_buf || s_jpeg_buf_size == 0) return MAKERESULT(0x1FF, 10); /* no JPEG buffer */
    u64 jpeg_size = 0;
    s_jpeg_last_size = 0;
    Result rc = capsscCaptureJpegScreenShot(&jpeg_size, s_jpeg_buf, s_jpeg_buf_size, stack, (s64)timeout_ns);
    if (R_SUCCEEDED(rc)) {
        bool size_ok = jpeg_size > 0 && jpeg_size <= s_jpeg_buf_size;
        int d = !size_ok ? SAFIR_JPEG_E_ARG
                : out_gray ? safir_jpeg_crop_gray(s_jpeg_buf, (size_t)jpeg_size, rect, out_gray, NULL, NULL)
                           : 0; /* JPEG only (cloud OCR): no decode */
        if (d != 0) rc = MAKERESULT(0x1FF, 11); /* JPEG decode failed */
        else s_jpeg_last_size = (size_t)jpeg_size;
    }
    return rc;
}

static char s_capture_diag[192] = "";
static int s_good_attempt = -1; /* attempt that last succeeded; tried first next time */

const char *safir_capture_last_diag(void) {
    return s_capture_diag;
}

/*
 * The raw-screenshot stream needs debug mode: on retail units it fails with 2206-0820
 * (seen on HOS 22.5.0). The JPEG screenshot call works without it, so it goes first;
 * the raw stream stays as a last resort. The attempt that works is remembered, and
 * every result code is recorded for the UI. In the diag, 2511-xxxx codes are safir's
 * own: 0010 no JPEG buffer (heap OOM), 0011 JPEG decode error, 0012 firmware < 10.0.0.
 */
Result safir_capture_crop_grayscale(uint8_t *out_gray, SafirRect rect, u64 timeout_ns) {
    if (!out_gray || rect.w == 0 || rect.h == 0) return MAKERESULT(0x1FF, 1);
    if ((u32)rect.x + rect.w > 1280 || (u32)rect.y + rect.h > 720) return MAKERESULT(0x1FF, 2);

    static const struct { bool jpeg; ViLayerStack stack; const char *name; u64 timeout_ns; } attempts[] = {
        { true,  ViLayerStack_Screenshot, "JpgShot", 1000000000ULL },
        { true,  ViLayerStack_Default,    "JpgDef",  1000000000ULL },
        { false, ViLayerStack_Default,    "Raw",     0 },            /* debug-mode units only */
    };
    const int n = (int)(sizeof(attempts) / sizeof(attempts[0]));

    size_t used = 0;
    s_capture_diag[0] = '\0';
    Result last = MAKERESULT(0x1FF, 5);
    for (int k = -1; k < n; ++k) {
        int i = (k < 0) ? s_good_attempt : k;
        if (i < 0 || (k >= 0 && i == s_good_attempt)) continue;
        u64 t = attempts[i].timeout_ns ? attempts[i].timeout_ns : timeout_ns;
        Result rc = attempts[i].jpeg ? capture_jpeg(out_gray, rect, attempts[i].stack, t)
                                     : capture_once(out_gray, rect, attempts[i].stack, t);
        if (R_SUCCEEDED(rc)) {
            s_good_attempt = i;
            if (used < sizeof(s_capture_diag)) {
                snprintf(s_capture_diag + used, sizeof(s_capture_diag) - used, "ok:%s", attempts[i].name);
            }
            return 0;
        }
        last = rc;
        if (used < sizeof(s_capture_diag)) {
            int w = snprintf(s_capture_diag + used, sizeof(s_capture_diag) - used, "%s=%04u-%04u ",
                             attempts[i].name, 2000u + R_MODULE(rc), R_DESCRIPTION(rc));
            if (w > 0) used += (size_t)w;
        }
    }
    s_good_attempt = -1;
    return last;
}

#endif
