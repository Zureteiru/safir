#include "safir_layout.h"
#include <string.h>

static uint16_t s_touch_padding = SAFIR_DEFAULT_TOUCH_PADDING;
static SafirCursorState s_active_cursor;
static bool s_active_cursor_valid = false;

void safir_layout_set_touch_padding(uint16_t padding_px) {
    s_touch_padding = padding_px;
}

uint16_t safir_layout_get_touch_padding(void) {
    return s_touch_padding;
}

int safir_hit_test_touch_padded(const SafirTextLine *lines, int line_count, uint16_t touch_x, uint16_t touch_y, uint16_t padding_px) {
    if (!lines || line_count <= 0) return SAFIR_HIT_NONE;

    int best_hit = SAFIR_HIT_NONE;
    int32_t min_edge_dist = 0x7FFFFFFF;
    int32_t min_center_dist = 0x7FFFFFFF;
    int32_t pad = (int32_t)padding_px;
    int32_t pad_sq = pad * pad;

    for (int l = 0; l < line_count; ++l) {
        const SafirTextLine *line = &lines[l];
        for (int c = 0; c < (int)line->char_count; ++c) {
            const SafirRect *box = &line->chars[c].screen_box;
            if (box->w == 0 || box->h == 0) continue;

            int32_t box_left = (int32_t)box->x;
            int32_t box_right = box_left + (int32_t)box->w;
            int32_t box_top = (int32_t)box->y;
            int32_t box_bottom = box_top + (int32_t)box->h;

            int32_t dx = 0;
            if ((int32_t)touch_x < box_left) {
                dx = box_left - (int32_t)touch_x;
            } else if ((int32_t)touch_x > box_right) {
                dx = (int32_t)touch_x - box_right;
            }

            int32_t dy = 0;
            if ((int32_t)touch_y < box_top) {
                dy = box_top - (int32_t)touch_y;
            } else if ((int32_t)touch_y > box_bottom) {
                dy = (int32_t)touch_y - box_bottom;
            }

            if (dx <= pad && dy <= pad) {
                int32_t edge_dist_sq = dx * dx + dy * dy;
                if (edge_dist_sq <= pad_sq) {
                    int32_t cx = box_left + ((int32_t)box->w / 2);
                    int32_t cy = box_top + ((int32_t)box->h / 2);
                    int32_t cdx = (int32_t)touch_x - cx;
                    int32_t cdy = (int32_t)touch_y - cy;
                    int32_t center_dist_sq = cdx * cdx + cdy * cdy;

                    if (best_hit == SAFIR_HIT_NONE ||
                        edge_dist_sq < min_edge_dist ||
                        (edge_dist_sq == min_edge_dist && center_dist_sq < min_center_dist)) {
                        min_edge_dist = edge_dist_sq;
                        min_center_dist = center_dist_sq;
                        best_hit = safir_hit_pack(l, c);
                    }
                }
            }
        }
    }

    return best_hit;
}

int safir_hit_test_touch(const SafirTextLine *lines, int line_count, uint16_t touch_x, uint16_t touch_y) {
    return safir_hit_test_touch_padded(lines, line_count, touch_x, touch_y, s_touch_padding);
}

bool safir_hit_test_touch_pos(const SafirTextLine *lines, int line_count, uint16_t touch_x, uint16_t touch_y, uint16_t padding_px, int *out_line, int *out_char) {
    int hit = safir_hit_test_touch_padded(lines, line_count, touch_x, touch_y, padding_px);
    if (hit < 0) {
        if (out_line) *out_line = -1;
        if (out_char) *out_char = -1;
        return false;
    }
    if (out_line) *out_line = safir_hit_line(hit);
    if (out_char) *out_char = safir_hit_char(hit);
    return true;
}

SafirRect safir_compute_popup_rect(SafirRect active_word_box, uint16_t popup_w, uint16_t popup_h) {
    uint16_t max_allowed_w = SAFIR_CLAMP_MAX_X - SAFIR_CLAMP_MIN_X; /* 1248 */
    uint16_t max_allowed_h = SAFIR_CLAMP_MAX_Y - SAFIR_CLAMP_MIN_Y; /* 688 */

    uint16_t w = (popup_w > max_allowed_w) ? max_allowed_w : popup_w;
    uint16_t h = (popup_h > max_allowed_h) ? max_allowed_h : popup_h;

    int32_t word_center_x = (int32_t)active_word_box.x + ((int32_t)active_word_box.w / 2);
    int32_t word_center_y = (int32_t)active_word_box.y + ((int32_t)active_word_box.h / 2);

    int32_t ideal_x = word_center_x - ((int32_t)w / 2);
    int32_t ideal_y;

    /*
     * If dialogue box is in the bottom half of 1280x720 (center_y >= 360),
     * place the popup directly ABOVE the word.
     * Otherwise, place BELOW the word.
     */
    if (word_center_y >= (SAFIR_SCREEN_HEIGHT / 2)) {
        ideal_y = (int32_t)active_word_box.y - (int32_t)h - SAFIR_POPUP_GAP_Y;
    } else {
        ideal_y = (int32_t)active_word_box.y + (int32_t)active_word_box.h + SAFIR_POPUP_GAP_Y;
    }

    /* Strictly clamp within [16, 1264] x [16, 704] */
    int32_t min_x = SAFIR_CLAMP_MIN_X;
    int32_t max_x = (int32_t)SAFIR_CLAMP_MAX_X - (int32_t)w;
    int32_t clamped_x = ideal_x;
    if (clamped_x < min_x) clamped_x = min_x;
    if (clamped_x > max_x) clamped_x = max_x;

    int32_t min_y = SAFIR_CLAMP_MIN_Y;
    int32_t max_y = (int32_t)SAFIR_CLAMP_MAX_Y - (int32_t)h;
    int32_t clamped_y = ideal_y;
    if (clamped_y < min_y) clamped_y = min_y;
    if (clamped_y > max_y) clamped_y = max_y;

    return safir_rect_make((uint16_t)clamped_x, (uint16_t)clamped_y, w, h);
}

void safir_cursor_init(SafirCursorState *state, const SafirTextLine *lines, int line_count, SafirDict *dict) {
    if (!state) return;
    state->lines = lines;
    state->line_count = line_count;
    state->dict = dict;
    state->cur_line = 0;
    state->cur_char = 0;
    state->touch_padding = s_touch_padding;
}

void safir_layout_set_active_state(SafirCursorState *state) {
    if (state) {
        s_active_cursor = *state;
        s_active_cursor_valid = true;
    } else {
        s_active_cursor_valid = false;
    }
}

SafirCursorState *safir_layout_get_active_state(void) {
    return s_active_cursor_valid ? &s_active_cursor : NULL;
}

bool safir_cursor_state_step_char(SafirCursorState *state, int delta) {
    if (!state || !state->lines || state->line_count <= 0 || delta == 0) return false;

    if (state->cur_line < 0) state->cur_line = 0;
    if (state->cur_line >= state->line_count) state->cur_line = state->line_count - 1;

    int orig_line = state->cur_line;
    int orig_char = state->cur_char;

    if (delta > 0) {
        for (int step = 0; step < delta; ++step) {
            int line_chars = (int)state->lines[state->cur_line].char_count;
            if (state->cur_char + 1 < line_chars) {
                state->cur_char++;
            } else {
                int next_line = state->cur_line + 1;
                while (next_line < state->line_count && state->lines[next_line].char_count == 0) {
                    next_line++;
                }
                if (next_line < state->line_count) {
                    state->cur_line = next_line;
                    state->cur_char = 0;
                } else {
                    if (line_chars > 0) {
                        state->cur_char = line_chars - 1;
                    }
                    break;
                }
            }
        }
    } else {
        int steps = -delta;
        for (int step = 0; step < steps; ++step) {
            if (state->cur_char > 0) {
                state->cur_char--;
            } else {
                int prev_line = state->cur_line - 1;
                while (prev_line >= 0 && state->lines[prev_line].char_count == 0) {
                    prev_line--;
                }
                if (prev_line >= 0) {
                    state->cur_line = prev_line;
                    int prev_chars = (int)state->lines[prev_line].char_count;
                    state->cur_char = (prev_chars > 0) ? (prev_chars - 1) : 0;
                } else {
                    state->cur_char = 0;
                    break;
                }
            }
        }
    }

    return (state->cur_line != orig_line || state->cur_char != orig_char);
}

bool safir_cursor_state_switch_line(SafirCursorState *state, int delta) {
    if (!state || !state->lines || state->line_count <= 0 || delta == 0) return false;

    int new_line = state->cur_line + delta;
    if (new_line < 0) new_line = 0;
    if (new_line >= state->line_count) new_line = state->line_count - 1;

    if (new_line == state->cur_line) return false;

    state->cur_line = new_line;
    int line_chars = (int)state->lines[new_line].char_count;
    if (line_chars > 0 && state->cur_char >= line_chars) {
        state->cur_char = line_chars - 1;
    } else if (line_chars == 0) {
        state->cur_char = 0;
    }
    return true;
}

bool safir_cursor_state_next_word(SafirCursorState *state) {
    if (!state || !state->lines || state->line_count <= 0) return false;

    if (state->cur_line < 0) state->cur_line = 0;
    if (state->cur_line >= state->line_count) state->cur_line = state->line_count - 1;

    const SafirTextLine *cur_line_ptr = &state->lines[state->cur_line];
    if (cur_line_ptr->char_count == 0) {
        return safir_cursor_state_step_char(state, 1);
    }

    if (state->dict) {
        SafirLineTokens tokens;
        safir_dict_tokenize_line(state->dict, cur_line_ptr, &tokens);
        int next_idx = safir_tokens_next(&tokens, (uint16_t)state->cur_char);
        if (next_idx >= 0) {
            state->cur_char = next_idx;
            return true;
        }
    }

    /* If no further word on current line, transition to next non-empty line */
    int next_line = state->cur_line + 1;
    while (next_line < state->line_count && state->lines[next_line].char_count == 0) {
        next_line++;
    }
    if (next_line < state->line_count) {
        state->cur_line = next_line;
        state->cur_char = 0;
        return true;
    }

    /* At end: advance to last character if not already there */
    if (state->cur_char < (int)cur_line_ptr->char_count - 1) {
        state->cur_char = (int)cur_line_ptr->char_count - 1;
        return true;
    }

    return false;
}

bool safir_cursor_state_prev_word(SafirCursorState *state) {
    if (!state || !state->lines || state->line_count <= 0) return false;

    if (state->cur_line < 0) state->cur_line = 0;
    if (state->cur_line >= state->line_count) state->cur_line = state->line_count - 1;

    const SafirTextLine *cur_line_ptr = &state->lines[state->cur_line];
    if (cur_line_ptr->char_count == 0) {
        return safir_cursor_state_step_char(state, -1);
    }

    if (state->dict) {
        SafirLineTokens tokens;
        safir_dict_tokenize_line(state->dict, cur_line_ptr, &tokens);
        int prev_idx = safir_tokens_prev(&tokens, (uint16_t)state->cur_char);
        if (prev_idx >= 0) {
            state->cur_char = prev_idx;
            return true;
        }
    }

    /* If no prior word on this line, transition to previous non-empty line */
    int prev_line = state->cur_line - 1;
    while (prev_line >= 0 && state->lines[prev_line].char_count == 0) {
        prev_line--;
    }
    if (prev_line >= 0) {
        state->cur_line = prev_line;
        const SafirTextLine *p_line = &state->lines[prev_line];
        if (state->dict) {
            SafirLineTokens tokens;
            safir_dict_tokenize_line(state->dict, p_line, &tokens);
            if (tokens.token_count > 0) {
                state->cur_char = tokens.tokens[tokens.token_count - 1].start_char_idx;
            } else {
                state->cur_char = (p_line->char_count > 0) ? ((int)p_line->char_count - 1) : 0;
            }
        } else {
            state->cur_char = (p_line->char_count > 0) ? ((int)p_line->char_count - 1) : 0;
        }
        return true;
    }

    /* At beginning: retreat to 0 if not already there */
    if (state->cur_char > 0) {
        state->cur_char = 0;
        return true;
    }

    return false;
}

SafirRect safir_cursor_state_get_char_box(const SafirCursorState *state) {
    if (!state || !state->lines || state->line_count <= 0) {
        return safir_rect_make(0, 0, 0, 0);
    }
    if (state->cur_line < 0 || state->cur_line >= state->line_count) {
        return safir_rect_make(0, 0, 0, 0);
    }
    const SafirTextLine *line = &state->lines[state->cur_line];
    if (state->cur_char < 0 || state->cur_char >= (int)line->char_count) {
        return safir_rect_make(0, 0, 0, 0);
    }
    return line->chars[state->cur_char].screen_box;
}

SafirRect safir_cursor_state_get_word_box(SafirCursorState *state, uint16_t *out_char_count) {
    if (out_char_count) *out_char_count = 1;
    if (!state || !state->lines || state->line_count <= 0) {
        return safir_rect_make(0, 0, 0, 0);
    }
    if (state->cur_line < 0 || state->cur_line >= state->line_count) {
        return safir_rect_make(0, 0, 0, 0);
    }
    const SafirTextLine *line = &state->lines[state->cur_line];
    if (state->cur_char < 0 || state->cur_char >= (int)line->char_count) {
        return safir_rect_make(0, 0, 0, 0);
    }

    uint16_t count = 1;
    if (state->dict) {
        SafirLookupResult match;
        if (safir_dict_scan_longest(state->dict, line, (uint16_t)state->cur_char, &match) && match.matched_chars > 0) {
            count = match.matched_chars;
        }
    }
    if (out_char_count) *out_char_count = count;

    /* Combine bounding boxes from cur_char to cur_char + count - 1 */
    SafirRect first_box = line->chars[state->cur_char].screen_box;
    uint16_t min_x = first_box.x;
    uint16_t min_y = first_box.y;
    uint16_t max_x = first_box.x + first_box.w;
    uint16_t max_y = first_box.y + first_box.h;

    for (uint16_t i = 1; i < count; ++i) {
        int idx = state->cur_char + i;
        if (idx >= (int)line->char_count) break;
        const SafirRect *b = &line->chars[idx].screen_box;
        if (b->x < min_x) min_x = b->x;
        if (b->y < min_y) min_y = b->y;
        if (b->x + b->w > max_x) max_x = b->x + b->w;
        if (b->y + b->h > max_y) max_y = b->y + b->h;
    }

    return safir_rect_make(min_x, min_y, max_x - min_x, max_y - min_y);
}

/* --- Global convenience functions matching prompt names --- */

bool cursor_next_word(void) {
    if (!s_active_cursor_valid) return false;
    return safir_cursor_state_next_word(&s_active_cursor);
}

bool cursor_prev_word(void) {
    if (!s_active_cursor_valid) return false;
    return safir_cursor_state_prev_word(&s_active_cursor);
}

bool cursor_step_char(int delta) {
    if (!s_active_cursor_valid) return false;
    return safir_cursor_state_step_char(&s_active_cursor, delta);
}

bool cursor_switch_line(int delta) {
    if (!s_active_cursor_valid) return false;
    return safir_cursor_state_switch_line(&s_active_cursor, delta);
}
