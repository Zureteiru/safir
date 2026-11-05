#ifndef SAFIR_OVERLAY_UI_H
#define SAFIR_OVERLAY_UI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "safir_types.h"
#include "safir_dict.h"
#include "safir_ocr.h"
#include "safir_layout.h"
#include "safir_text.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SafirUiMode {
    SAFIR_UI_HIDDEN        = 0,
    SAFIR_UI_POPUP         = 1,
    SAFIR_UI_REGION_EDITOR = 2,
    SAFIR_UI_SETTINGS      = 3
} SafirUiMode;

typedef enum SafirOcrEngineChoice {
    SAFIR_ENGINE_LOCAL_VOCR6   = 0,
    SAFIR_ENGINE_GOOGLE_VISION = 1
} SafirOcrEngineChoice;

typedef struct SafirOverlayConfig {
    const char *games_ini_path;
    uint64_t title_id;
} SafirOverlayConfig;

/**
 * Initializes overlay UI state machine with dictionary and OCR backends.
 */
bool safir_ui_init(SafirDict *dict,
                   SafirOcrBackend *local_ocr,
                   SafirOcrBackend *cloud_ocr,
                   const SafirOverlayConfig *config);

/**
 * Updates overlay UI state machine based on controller inputs and touch.
 */
void safir_ui_update(uint64_t buttons_down,
                     uint64_t buttons_held,
                     int stick_lx,
                     int stick_ly,
                     int stick_rx,
                     int stick_ry,
                     int touch_count,
                     uint32_t touch_x,
                     uint32_t touch_y);

/**
 * Renders overlay UI frame.
 */
void safir_ui_render(void);

/**
 * Returns current UI mode.
 */
SafirUiMode safir_ui_get_mode(void);

/**
 * Explicitly sets the UI mode.
 */
void safir_ui_set_mode(SafirUiMode mode);

/**
 * Returns true if overlay should remain loaded and running.
 * Becomes false when user selects 'Unload & Exit Overlay'.
 */
bool safir_ui_is_running(void);

/**
 * Requests overlay shutdown and cleanup.
 */
void safir_ui_request_exit(void);

/**
 * Returns active dialogue box rectangle.
 */
SafirRect safir_ui_get_dialogue_rect(void);

/**
 * Sets active dialogue box rectangle.
 */
void safir_ui_set_dialogue_rect(SafirRect rect);

/**
 * Retrieves the currently active toast message and remaining duration in ms.
 * Returns true if toast is active.
 */
bool safir_ui_get_toast(char *out_buf, size_t max_buf, uint32_t *out_remaining_ms);

/**
 * Sets an active toast message with duration in ms.
 */
void safir_ui_set_toast(const char *msg, uint32_t duration_ms);

/**
 * Gets currently selected OCR engine choice.
 */
SafirOcrEngineChoice safir_ui_get_engine_choice(void);

/**
 * Sets OCR engine choice.
 */
void safir_ui_set_engine_choice(SafirOcrEngineChoice choice);

/**
 * Retrieves the current cursor state.
 */
const SafirCursorState *safir_ui_get_cursor_state(void);

/**
 * Retrieves the active lookup result.
 */
bool safir_ui_get_active_lookup(SafirLookupResult *out_lookup);

/**
 * Retrieves current popup placement rect.
 */
SafirRect safir_ui_get_popup_rect(void);

/**
 * Query whether the current OCR result in popup was from the 0ms background cache.
 */
bool safir_ui_is_current_ocr_cached(void);

/**
 * Cleans up overlay UI state.
 */
void safir_ui_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* SAFIR_OVERLAY_UI_H */
