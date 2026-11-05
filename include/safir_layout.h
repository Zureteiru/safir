#ifndef SAFIR_LAYOUT_H
#define SAFIR_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "safir_types.h"
#include "safir_dict.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAFIR_DEFAULT_TOUCH_PADDING 12
#define SAFIR_POPUP_GAP_Y           8

/* Screen boundaries for popup clamping */
#define SAFIR_CLAMP_MIN_X 16
#define SAFIR_CLAMP_MAX_X 1264
#define SAFIR_CLAMP_MIN_Y 16
#define SAFIR_CLAMP_MAX_Y 704

#define SAFIR_HIT_NONE (-1)

static inline int safir_hit_pack(int line_idx, int char_idx) {
    if (line_idx < 0 || char_idx < 0) return SAFIR_HIT_NONE;
    return ((line_idx & 0x7FFF) << 16) | (char_idx & 0xFFFF);
}

static inline int safir_hit_line(int hit) {
    if (hit < 0) return -1;
    return (hit >> 16) & 0x7FFF;
}

static inline int safir_hit_char(int hit) {
    if (hit < 0) return -1;
    return hit & 0xFFFF;
}

/**
 * Cursor navigation state.
 */
typedef struct SafirCursorState {
    const SafirTextLine *lines;
    int line_count;
    SafirDict *dict;
    int cur_line;
    int cur_char;
    uint16_t touch_padding;
} SafirCursorState;

/* --- Touch Hit-Testing --- */

/**
 * Sets the default forgiveness padding in pixels for touch hit-testing (default: 12px).
 */
void safir_layout_set_touch_padding(uint16_t padding_px);

/**
 * Gets the current forgiveness padding in pixels.
 */
uint16_t safir_layout_get_touch_padding(void);

/**
 * Hit-tests a touch point (touch_x, touch_y) against recognition lines.
 * Returns packed (line_idx << 16 | char_idx), or SAFIR_HIT_NONE (-1) if no hit.
 * Uses configurable forgiveness padding (default 12px).
 */
int safir_hit_test_touch(const SafirTextLine *lines, int line_count, uint16_t touch_x, uint16_t touch_y);

/**
 * Hit-tests with an explicit padding distance in pixels.
 */
int safir_hit_test_touch_padded(const SafirTextLine *lines, int line_count, uint16_t touch_x, uint16_t touch_y, uint16_t padding_px);

/**
 * Convenience hit-tester unpacking line and char indices into output pointers.
 * Returns true if a character box was hit within padding.
 */
bool safir_hit_test_touch_pos(const SafirTextLine *lines, int line_count, uint16_t touch_x, uint16_t touch_y, uint16_t padding_px, int *out_line, int *out_char);


/* --- Smart Popup Placement --- */

/**
 * Places the dictionary popup card directly above the highlighted word if the dialogue box
 * is in the bottom half of the 1280x720 screen, or below it if the text is near the top.
 * Clamps strictly within [16, 1264] x [16, 704].
 */
SafirRect safir_compute_popup_rect(SafirRect active_word_box, uint16_t popup_w, uint16_t popup_h);


/* --- Cursor Navigation --- */

/**
 * Initializes cursor navigation context for the active UI state.
 */
void safir_cursor_init(SafirCursorState *state, const SafirTextLine *lines, int line_count, SafirDict *dict);

/**
 * Sets the active global cursor navigation state used by parameterless cursor_* functions.
 */
void safir_layout_set_active_state(SafirCursorState *state);

/**
 * Gets the active global cursor navigation state.
 */
SafirCursorState *safir_layout_get_active_state(void);

/**
 * Steps character cursor by delta (-1 left, +1 right) across lines.
 */
bool safir_cursor_state_step_char(SafirCursorState *state, int delta);

/**
 * Switches line by delta (-1 up, +1 down).
 */
bool safir_cursor_state_switch_line(SafirCursorState *state, int delta);

/**
 * Moves cursor to the start of the next word token.
 */
bool safir_cursor_state_next_word(SafirCursorState *state);

/**
 * Moves cursor to the start of the previous word token.
 */
bool safir_cursor_state_prev_word(SafirCursorState *state);

/**
 * Returns the screen bounding box of the active character under cursor.
 */
SafirRect safir_cursor_state_get_char_box(const SafirCursorState *state);

/**
 * Returns the combined bounding box of the word under cursor and character length.
 */
SafirRect safir_cursor_state_get_word_box(SafirCursorState *state, uint16_t *out_char_count);


/* --- Convenience Global Cursor Functions (Prompt Exact Signatures) --- */

bool cursor_next_word(void);
bool cursor_prev_word(void);
bool cursor_step_char(int delta);
bool cursor_switch_line(int delta);

#ifdef __cplusplus
}
#endif

#endif /* SAFIR_LAYOUT_H */
