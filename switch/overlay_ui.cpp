#include "overlay_ui.h"
#include "bg_worker.h"
#include "games_ini.h"
#include "mock_libnx.h"
#include "safir_crop.h"
#include <cstdio>
#include <cstring>
#include <cinttypes>

static SafirUiMode s_ui_mode = SAFIR_UI_HIDDEN;
static bool s_running = true;

static SafirDict *s_dict = nullptr;
static SafirOcrBackend *s_local_ocr = nullptr;
static SafirOcrBackend *s_cloud_ocr = nullptr;
static SafirOcrEngineChoice s_engine_choice = SAFIR_ENGINE_LOCAL_VOCR6;

static SafirRect s_dialogue_rect;
static SafirRect s_editor_rect;
static uint64_t s_title_id = 0x0100ABCD12340000ULL;

static char s_games_ini_path[256];

static SafirOcrResult s_active_ocr;
static uint64_t s_active_dhash = 0;
static SafirCursorState s_cursor_state;
static SafirLookupResult s_active_lookup;
static bool s_has_lookup = false;
static SafirRect s_popup_rect;

/* Quick Settings Drawer State */
static int s_settings_selection = 0;

/* Toast State */
static char s_toast_msg[128];
static uint32_t s_toast_remaining_ms = 0;

/* Dialogue bounds */
#define UI_MAX_CROP_W 1280
#define UI_MAX_CROP_H 720

/* Helper to convert UTF-32 codepoint to UTF-8 */
static size_t cp_to_utf8(uint32_t cp, char *out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

static SafirOcrBackend *get_active_ocr_backend() {
    return (s_engine_choice == SAFIR_ENGINE_LOCAL_VOCR6) ? s_local_ocr : s_cloud_ocr;
}

static void update_lookup_and_popup() {
    s_has_lookup = false;
    memset(&s_active_lookup, 0, sizeof(s_active_lookup));

    if (!s_dict || s_cursor_state.line_count <= 0) {
        return;
    }

    int line_idx = s_cursor_state.cur_line;
    int char_idx = s_cursor_state.cur_char;
    if (line_idx < 0 || line_idx >= s_cursor_state.line_count) return;

    const SafirTextLine *line = &s_cursor_state.lines[line_idx];
    if (char_idx < 0 || char_idx >= (int)line->char_count) return;

    /* Longest-match lookup with deinflection via dictionary */
    safir_engine_lock();
    bool found = safir_dict_scan_longest(s_dict, line, (uint16_t)char_idx, &s_active_lookup);
    safir_engine_unlock();
    if (found) {
        s_has_lookup = true;
    } else {
        /* Single char fallback */
        char single_char[8];
        size_t n = cp_to_utf8(line->chars[char_idx].codepoint, single_char);
        single_char[n] = '\0';
        strncpy(s_active_lookup.headword, single_char, sizeof(s_active_lookup.headword) - 1);
        strncpy(s_active_lookup.glossary, "(No definition found)", sizeof(s_active_lookup.glossary) - 1);
        s_has_lookup = true;
    }

    uint16_t word_chars = 1;
    SafirRect word_box = safir_cursor_state_get_word_box(&s_cursor_state, &word_chars);
    s_popup_rect = safir_compute_popup_rect(word_box, 360, 160);
}

static bool run_on_demand_ocr_locked();

static bool run_on_demand_ocr() {
    safir_engine_lock();
    bool ok = run_on_demand_ocr_locked();
    safir_engine_unlock();
    return ok;
}

static bool run_on_demand_ocr_locked() {
    SafirOcrBackend *backend = get_active_ocr_backend();
    if (!backend || !backend->recognize) return false;

    SafirRect rect = s_dialogue_rect;
    if (rect.w == 0 || rect.h == 0 || rect.w > UI_MAX_CROP_W || rect.h > UI_MAX_CROP_H) {
        return false;
    }

    uint16_t crop_w = (rect.w > 0) ? ((rect.w + 1) / 2) : 550;
    uint16_t crop_h = (rect.h > 0) ? ((rect.h + 1) / 2) : 105;
    size_t num_pixels = (size_t)crop_w * crop_h;
    safir_pool_reset(POOL_OCR_SCRATCH);
    uint8_t *crop_gray = safir_acquire_crop(num_pixels);
    if (!crop_gray) return false;

    Result rc = safir_capture_crop_grayscale(crop_gray, rect, 100000000ULL);
    if (R_FAILED(rc)) return false;

    safir_crop_auto_invert(crop_gray, crop_w, crop_h);
    uint64_t dhash = safir_compute_dhash(crop_gray, crop_w, crop_h);

    SafirOcrResult res;
    memset(&res, 0, sizeof(res));
    if (!backend->recognize(backend, crop_gray, crop_w, crop_h, rect, &res)) {
        return false;
    }

    res.dhash = dhash;
    res.cached = false;
    s_active_ocr = res;
    s_active_dhash = dhash;
    return true;
}

bool safir_ui_init(SafirDict *dict,
                   SafirOcrBackend *local_ocr,
                   SafirOcrBackend *cloud_ocr,
                   const SafirOverlayConfig *config) {
    s_dict = dict;
    s_local_ocr = local_ocr;
    s_cloud_ocr = cloud_ocr;
    s_engine_choice = SAFIR_ENGINE_LOCAL_VOCR6;

    if (config) {
        s_title_id = config->title_id ? config->title_id : 0x0100ABCD12340000ULL;
        strncpy(s_games_ini_path, config->games_ini_path ? config->games_ini_path : SAFIR_GAMES_INI_DEFAULT_PATH, sizeof(s_games_ini_path) - 1);
    } else {
        s_title_id = 0x0100ABCD12340000ULL;
        strncpy(s_games_ini_path, SAFIR_GAMES_INI_DEFAULT_PATH, sizeof(s_games_ini_path) - 1);
    }

    /* Load game dialogue rect from INI or fall back to default */
    if (!safir_games_ini_load(s_games_ini_path, s_title_id, &s_dialogue_rect)) {
        s_dialogue_rect = safir_default_dialogue_rect();
    }
    s_editor_rect = s_dialogue_rect;

    safir_bg_worker_set_dialogue_rect(s_dialogue_rect);

    s_ui_mode = SAFIR_UI_HIDDEN;
    requestForeground(false);
    s_running = true;
    s_settings_selection = 0;
    s_toast_remaining_ms = 0;
    s_toast_msg[0] = '\0';

    return true;
}

void safir_ui_update(uint64_t buttons_down,
                     uint64_t buttons_held,
                     int stick_lx,
                     int stick_ly,
                     int stick_rx,
                     int stick_ry,
                     int touch_count,
                     uint32_t touch_x,
                     uint32_t touch_y) {
    (void)stick_ly;

    /* Update toast countdown */
    if (s_toast_remaining_ms > 0) {
        if (s_toast_remaining_ms <= 16) {
            s_toast_remaining_ms = 0;
            s_toast_msg[0] = '\0';
        } else {
            s_toast_remaining_ms -= 16;
        }
    }

    if (s_ui_mode == SAFIR_UI_HIDDEN) {
        /* Instant Popup Trigger: L + R or ZL + ZR */
        bool trigger_lr = ((buttons_held & (HidNpadButton_L | HidNpadButton_R)) == (HidNpadButton_L | HidNpadButton_R)) &&
                          ((buttons_down & (HidNpadButton_L | HidNpadButton_R)) != 0);
        bool trigger_zlzr = ((buttons_held & (HidNpadButton_ZL | HidNpadButton_ZR)) == (HidNpadButton_ZL | HidNpadButton_ZR)) &&
                            ((buttons_down & (HidNpadButton_ZL | HidNpadButton_ZR)) != 0);

        if (trigger_lr || trigger_zlzr) {
            requestForeground(true);
            s_ui_mode = SAFIR_UI_POPUP;

            bool got_cached = false;
            if (safir_bg_worker_is_enabled()) {
                SafirOcrResult cached_res;
                uint64_t cached_hash = 0;
                if (safir_bg_worker_get_cached_result(&cached_res, &cached_hash)) {
                    s_active_ocr = cached_res;
                    s_active_dhash = cached_hash;
                    s_active_ocr.cached = true;
                    got_cached = true;
                }
            }

            if (!got_cached) {
                /* Background mode is OFF or cache empty: run OCR on-demand right when L+R pressed */
                run_on_demand_ocr();
            }

            safir_engine_lock();
            safir_cursor_init(&s_cursor_state, s_active_ocr.lines, s_active_ocr.line_count, s_dict);
            safir_engine_unlock();
            safir_layout_set_active_state(&s_cursor_state);
            update_lookup_and_popup();
        }
        return;
    }

    if (s_ui_mode == SAFIR_UI_POPUP) {
        /* Plus (+): Open Quick Settings drawer */
        if (buttons_down & HidNpadButton_Plus) {
            s_ui_mode = SAFIR_UI_SETTINGS;
            s_settings_selection = 0;
            return;
        }

        /* Square (Y on Switch): Enter Region Box Editor */
        if (buttons_down & HidNpadButton_Y) {
            s_ui_mode = SAFIR_UI_REGION_EDITOR;
            s_editor_rect = s_dialogue_rect;
            return;
        }

        /* B: Close popup mode and return to game */
        if (buttons_down & HidNpadButton_B) {
            s_ui_mode = SAFIR_UI_HIDDEN;
            requestForeground(false);
            return;
        }

        /* D-Pad Left / Right: Jump word-by-word */
        if (buttons_down & HidNpadButton_Left) {
            safir_cursor_state_prev_word(&s_cursor_state);
            update_lookup_and_popup();
            return;
        }
        if (buttons_down & HidNpadButton_Right) {
            safir_cursor_state_next_word(&s_cursor_state);
            update_lookup_and_popup();
            return;
        }

        /* Left Stick Left / Right: Scrub character-by-character */
        if ((buttons_down & HidNpadButton_StickLLeft) || (stick_lx < -16000)) {
            safir_cursor_state_step_char(&s_cursor_state, -1);
            update_lookup_and_popup();
            return;
        }
        if ((buttons_down & HidNpadButton_StickLRight) || (stick_lx > 16000)) {
            safir_cursor_state_step_char(&s_cursor_state, 1);
            update_lookup_and_popup();
            return;
        }

        /* Touchscreen Tap: Jump cursor directly to tapped character and open popup */
        if (touch_count > 0) {
            int hit_line = -1;
            int hit_char = -1;
            if (safir_hit_test_touch_pos(s_active_ocr.lines, s_active_ocr.line_count, (uint16_t)touch_x, (uint16_t)touch_y, safir_layout_get_touch_padding(), &hit_line, &hit_char)) {
                s_cursor_state.cur_line = hit_line;
                s_cursor_state.cur_char = hit_char;
                update_lookup_and_popup();
            }
            return;
        }

        return;
    }

    if (s_ui_mode == SAFIR_UI_REGION_EDITOR) {
        /* Move with Left Stick or D-Pad */
        uint16_t step = 5;
        if ((buttons_down & HidNpadButton_Left) || stick_lx < -16000) {
            if (s_editor_rect.x >= step) s_editor_rect.x -= step;
            else s_editor_rect.x = 0;
        }
        if ((buttons_down & HidNpadButton_Right) || stick_lx > 16000) {
            if (s_editor_rect.x + s_editor_rect.w + step <= SAFIR_SCREEN_WIDTH) s_editor_rect.x += step;
        }
        if ((buttons_down & HidNpadButton_Up) || stick_ly < -16000) {
            if (s_editor_rect.y >= step) s_editor_rect.y -= step;
            else s_editor_rect.y = 0;
        }
        if ((buttons_down & HidNpadButton_Down) || stick_ly > 16000) {
            if (s_editor_rect.y + s_editor_rect.h + step <= SAFIR_SCREEN_HEIGHT) s_editor_rect.y += step;
        }

        /* Resize with Right Stick */
        if ((buttons_down & HidNpadButton_ZL) || stick_rx < -16000) {
            if (s_editor_rect.w > 100 + step) s_editor_rect.w -= step;
            else s_editor_rect.w = 100;
        }
        if ((buttons_down & HidNpadButton_ZR) || stick_rx > 16000) {
            if (s_editor_rect.x + s_editor_rect.w + step <= SAFIR_SCREEN_WIDTH && s_editor_rect.w + step <= UI_MAX_CROP_W) s_editor_rect.w += step;
        }
        if ((buttons_down & HidNpadButton_L) || stick_ry < -16000) {
            if (s_editor_rect.h > 40 + step) s_editor_rect.h -= step;
            else s_editor_rect.h = 40;
        }
        if ((buttons_down & HidNpadButton_R) || stick_ry > 16000) {
            if (s_editor_rect.y + s_editor_rect.h + step <= SAFIR_SCREEN_HEIGHT && s_editor_rect.h + step <= UI_MAX_CROP_H) s_editor_rect.h += step;
        }

        /* Touch dragging adjustment in editor */
        if (touch_count > 0 && touch_x > 0 && touch_y > 0) {
            if (touch_x < SAFIR_SCREEN_WIDTH && touch_y < SAFIR_SCREEN_HEIGHT) {
                uint16_t center_x = (uint16_t)touch_x;
                uint16_t center_y = (uint16_t)touch_y;
                if (center_x > s_editor_rect.w / 2 && center_x + s_editor_rect.w / 2 <= SAFIR_SCREEN_WIDTH) {
                    s_editor_rect.x = center_x - (s_editor_rect.w / 2);
                }
                if (center_y > s_editor_rect.h / 2 && center_y + s_editor_rect.h / 2 <= SAFIR_SCREEN_HEIGHT) {
                    s_editor_rect.y = center_y - (s_editor_rect.h / 2);
                }
            }
        }

        /* A or Y: Save region to games.ini and apply */
        if (buttons_down & (HidNpadButton_A | HidNpadButton_Y)) {
            s_dialogue_rect = s_editor_rect;
            safir_games_ini_save(s_games_ini_path, s_title_id, s_dialogue_rect);
            safir_bg_worker_set_dialogue_rect(s_dialogue_rect);
            safir_ui_set_toast("Dialogue Region Saved!", 2000);
            s_ui_mode = SAFIR_UI_POPUP;
            return;
        }

        /* B: Cancel and return */
        if (buttons_down & HidNpadButton_B) {
            s_editor_rect = s_dialogue_rect;
            s_ui_mode = SAFIR_UI_POPUP;
            return;
        }

        return;
    }

    if (s_ui_mode == SAFIR_UI_SETTINGS) {
        /* Navigate options (0..2) */
        if (buttons_down & HidNpadButton_Up) {
            s_settings_selection = (s_settings_selection > 0) ? (s_settings_selection - 1) : 2;
        }
        if (buttons_down & HidNpadButton_Down) {
            s_settings_selection = (s_settings_selection < 2) ? (s_settings_selection + 1) : 0;
        }

        /* Activate option */
        if (buttons_down & (HidNpadButton_A | HidNpadButton_Left | HidNpadButton_Right)) {
            if (s_settings_selection == 0) {
                /* 1. Background Pre-OCR (Core 3): [ON / OFF] */
                bool cur = safir_bg_worker_is_enabled();
                safir_bg_worker_set_enabled(!cur);
            } else if (s_settings_selection == 1) {
                /* 2. OCR Engine: [Local VOCR6 / Google Fallback] */
                s_engine_choice = (s_engine_choice == SAFIR_ENGINE_LOCAL_VOCR6) ? SAFIR_ENGINE_GOOGLE_VISION : SAFIR_ENGINE_LOCAL_VOCR6;
                safir_bg_worker_set_backend(get_active_ocr_backend());
            } else if (s_settings_selection == 2) {
                /* 3. Unload & Exit Overlay */
                safir_bg_worker_stop();
                requestForeground(false);
                s_running = false;
                s_ui_mode = SAFIR_UI_HIDDEN;
                return;
            }
        }

        /* B: Return to popup mode */
        if (buttons_down & HidNpadButton_B) {
            s_ui_mode = SAFIR_UI_POPUP;
            return;
        }

        return;
    }
}

void safir_ui_render(void) {
    /* Virtual rendering representation */
    if (s_ui_mode == SAFIR_UI_HIDDEN) {
        return;
    }
}

SafirUiMode safir_ui_get_mode(void) {
    return s_ui_mode;
}

void safir_ui_set_mode(SafirUiMode mode) {
    s_ui_mode = mode;
    requestForeground(mode != SAFIR_UI_HIDDEN);
}

bool safir_ui_is_running(void) {
    return s_running;
}

void safir_ui_request_exit(void) {
    safir_bg_worker_stop();
    requestForeground(false);
    s_running = false;
    s_ui_mode = SAFIR_UI_HIDDEN;
}

SafirRect safir_ui_get_dialogue_rect(void) {
    return s_dialogue_rect;
}

void safir_ui_set_dialogue_rect(SafirRect rect) {
    s_dialogue_rect = rect;
    s_editor_rect = rect;
    safir_bg_worker_set_dialogue_rect(rect);
}

bool safir_ui_get_toast(char *out_buf, size_t max_buf, uint32_t *out_remaining_ms) {
    if (s_toast_remaining_ms == 0 || !out_buf || max_buf == 0) return false;
    strncpy(out_buf, s_toast_msg, max_buf - 1);
    out_buf[max_buf - 1] = '\0';
    if (out_remaining_ms) *out_remaining_ms = s_toast_remaining_ms;
    return true;
}

void safir_ui_set_toast(const char *msg, uint32_t duration_ms) {
    if (!msg) {
        s_toast_remaining_ms = 0;
        s_toast_msg[0] = '\0';
        return;
    }
    strncpy(s_toast_msg, msg, sizeof(s_toast_msg) - 1);
    s_toast_msg[sizeof(s_toast_msg) - 1] = '\0';
    s_toast_remaining_ms = duration_ms;
}

SafirOcrEngineChoice safir_ui_get_engine_choice(void) {
    return s_engine_choice;
}

void safir_ui_set_engine_choice(SafirOcrEngineChoice choice) {
    s_engine_choice = choice;
    safir_bg_worker_set_backend(get_active_ocr_backend());
}

const SafirCursorState *safir_ui_get_cursor_state(void) {
    return &s_cursor_state;
}

bool safir_ui_get_active_lookup(SafirLookupResult *out_lookup) {
    if (!s_has_lookup || !out_lookup) return false;
    *out_lookup = s_active_lookup;
    return true;
}

SafirRect safir_ui_get_popup_rect(void) {
    return s_popup_rect;
}

bool safir_ui_is_current_ocr_cached(void) {
    return s_active_ocr.cached;
}

void safir_ui_cleanup(void) {
    safir_bg_worker_stop();
    requestForeground(false);
    s_ui_mode = SAFIR_UI_HIDDEN;
    s_running = false;
}
