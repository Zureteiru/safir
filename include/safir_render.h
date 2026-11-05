#ifndef SAFIR_RENDER_H
#define SAFIR_RENDER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "safir_types.h"
#include "safir_dict.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Embedded 8x12 ASCII font glyph drawer */
void safir_fb_draw_char(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                        uint16_t x, uint16_t y, char c,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a, int scale);

void safir_fb_draw_string(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t x, uint16_t y, const char *str,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a, int scale);

/* Drawing primitives with alpha blending */
void safir_fb_fill_rect_blend(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                              SafirRect rect, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

void safir_fb_draw_rect_outline(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                                SafirRect rect, uint16_t thickness,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a);

void safir_fb_draw_rounded_rect(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                                SafirRect rect, uint16_t radius,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* Japanese Pitch Accent line diagram (High/Low mora bars and drop markers) */
void safir_fb_draw_pitch_accent(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                                uint16_t x, uint16_t y, const char *reading,
                                uint16_t pitch_mask, int scale);

/* Frequency rank pill badge */
void safir_fb_draw_freq_badge(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                              uint16_t x, uint16_t y, uint16_t freq_rank);

/* Comprehensive popup card */
void safir_fb_draw_popup_card(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                              SafirRect popup_rect, const SafirLookupResult *lookup);

/* Word highlight box drawer */
void safir_fb_draw_word_highlight(uint8_t *fb, uint16_t fb_w, uint16_t fb_h, SafirRect word_box);

/* Fast dirty-rect popup scene renderer (only touches word_box and popup_rect) */
void safir_fb_render_popup_dirty(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                                 SafirRect word_box, SafirRect popup_rect,
                                 const SafirLookupResult *lookup);

/* Export to BMP and PPM */
bool safir_fb_save_bmp(const uint8_t *rgba_fb, uint16_t w, uint16_t h, const char *filepath);
bool safir_fb_save_ppm(const uint8_t *rgba_fb, uint16_t w, uint16_t h, const char *filepath);

#ifdef __cplusplus
}
#endif

#endif /* SAFIR_RENDER_H */
