#include "safir_render.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ========================================================================= */
/* Embedded 8x12 Bitmap Font (ASCII 32..126)                                 */
/* ========================================================================= */

/* 5x7 glyph data expanded to 8x12 */
static const uint8_t s_font_8x12[95][12] = {
    {0,0,0,0,0,0,0,0,0,0,0,0},             /* ' ' (32) */
    {0,0,0x10,0x10,0x10,0x10,0x10,0,0x10,0,0,0}, /* '!' */
    {0,0x24,0x24,0x24,0,0,0,0,0,0,0,0},    /* '"' */
    {0,0,0x24,0x7E,0x24,0x24,0x7E,0x24,0,0,0,0}, /* '#' */
    {0,0x10,0x3C,0x50,0x38,0x14,0x78,0x10,0,0,0,0}, /* '$' */
    {0,0,0x62,0x64,0x08,0x10,0x26,0x46,0,0,0,0}, /* '%' */
    {0,0,0x30,0x48,0x30,0x4A,0x44,0x3A,0,0,0,0}, /* '&' */
    {0,0x10,0x10,0x20,0,0,0,0,0,0,0,0},    /* ''' */
    {0,0,0x08,0x10,0x20,0x20,0x10,0x08,0,0,0,0}, /* '(' */
    {0,0,0x20,0x10,0x08,0x08,0x10,0x20,0,0,0,0}, /* ')' */
    {0,0,0,0x14,0x08,0x3E,0x08,0x14,0,0,0,0}, /* '*' */
    {0,0,0,0x08,0x08,0x3E,0x08,0x08,0,0,0,0}, /* '+' */
    {0,0,0,0,0,0,0,0x10,0x10,0x20,0,0},    /* ',' */
    {0,0,0,0,0,0x3E,0,0,0,0,0,0},          /* '-' */
    {0,0,0,0,0,0,0,0x18,0x18,0,0,0},       /* '.' */
    {0,0,0x02,0x04,0x08,0x10,0x20,0x40,0,0,0,0}, /* '/' */
    {0,0,0x3C,0x46,0x4A,0x52,0x62,0x3C,0,0,0,0}, /* '0' */
    {0,0,0x18,0x28,0x08,0x08,0x08,0x3E,0,0,0,0}, /* '1' */
    {0,0,0x3C,0x42,0x04,0x18,0x20,0x7E,0,0,0,0}, /* '2' */
    {0,0,0x3C,0x42,0x18,0x04,0x42,0x3C,0,0,0,0}, /* '3' */
    {0,0,0x08,0x18,0x28,0x48,0x7E,0x08,0,0,0,0}, /* '4' */
    {0,0,0x7E,0x40,0x7C,0x02,0x42,0x3C,0,0,0,0}, /* '5' */
    {0,0,0x3C,0x40,0x7C,0x42,0x42,0x3C,0,0,0,0}, /* '6' */
    {0,0,0x7E,0x04,0x08,0x10,0x20,0x20,0,0,0,0}, /* '7' */
    {0,0,0x3C,0x42,0x3C,0x42,0x42,0x3C,0,0,0,0}, /* '8' */
    {0,0,0x3C,0x42,0x42,0x3E,0x02,0x3C,0,0,0,0}, /* '9' */
    {0,0,0,0x18,0x18,0,0x18,0x18,0,0,0,0}, /* ':' */
    {0,0,0,0x18,0x18,0,0x18,0x18,0x30,0,0,0}, /* ';' */
    {0,0,0x06,0x18,0x60,0x18,0x06,0,0,0,0,0}, /* '<' */
    {0,0,0,0x3E,0,0x3E,0,0,0,0,0,0},       /* '=' */
    {0,0,0x60,0x18,0x06,0x18,0x60,0,0,0,0,0}, /* '>' */
    {0,0,0x3C,0x42,0x04,0x08,0,0x08,0,0,0,0}, /* '?' */
    {0,0,0x3C,0x42,0x5A,0x5A,0x40,0x3E,0,0,0,0}, /* '@' */
    {0,0,0x3C,0x42,0x42,0x7E,0x42,0x42,0,0,0,0}, /* 'A' */
    {0,0,0x7C,0x42,0x7C,0x42,0x42,0x7C,0,0,0,0}, /* 'B' */
    {0,0,0x3C,0x42,0x40,0x40,0x42,0x3C,0,0,0,0}, /* 'C' */
    {0,0,0x78,0x44,0x42,0x42,0x44,0x78,0,0,0,0}, /* 'D' */
    {0,0,0x7E,0x40,0x7C,0x40,0x40,0x7E,0,0,0,0}, /* 'E' */
    {0,0,0x7E,0x40,0x7C,0x40,0x40,0x40,0,0,0,0}, /* 'F' */
    {0,0,0x3C,0x42,0x40,0x4E,0x42,0x3C,0,0,0,0}, /* 'G' */
    {0,0,0x42,0x42,0x7E,0x42,0x42,0x42,0,0,0,0}, /* 'H' */
    {0,0,0x3E,0x08,0x08,0x08,0x08,0x3E,0,0,0,0}, /* 'I' */
    {0,0,0x0E,0x02,0x02,0x02,0x42,0x3C,0,0,0,0}, /* 'J' */
    {0,0,0x44,0x48,0x70,0x48,0x44,0x42,0,0,0,0}, /* 'K' */
    {0,0,0x40,0x40,0x40,0x40,0x40,0x7E,0,0,0,0}, /* 'L' */
    {0,0,0x63,0x77,0x5D,0x49,0x41,0x41,0,0,0,0}, /* 'M' */
    {0,0,0x42,0x62,0x52,0x4A,0x46,0x42,0,0,0,0}, /* 'N' */
    {0,0,0x3C,0x42,0x42,0x42,0x42,0x3C,0,0,0,0}, /* 'O' */
    {0,0,0x7C,0x42,0x42,0x7C,0x40,0x40,0,0,0,0}, /* 'P' */
    {0,0,0x3C,0x42,0x42,0x4A,0x44,0x3A,0,0,0,0}, /* 'Q' */
    {0,0,0x7C,0x42,0x42,0x7C,0x44,0x42,0,0,0,0}, /* 'R' */
    {0,0,0x3C,0x42,0x38,0x04,0x42,0x3C,0,0,0,0}, /* 'S' */
    {0,0,0x7E,0x08,0x08,0x08,0x08,0x08,0,0,0,0}, /* 'T' */
    {0,0,0x42,0x42,0x42,0x42,0x42,0x3C,0,0,0,0}, /* 'U' */
    {0,0,0x42,0x42,0x42,0x24,0x24,0x18,0,0,0,0}, /* 'V' */
    {0,0,0x41,0x49,0x49,0x5B,0x77,0x63,0,0,0,0}, /* 'W' */
    {0,0,0x42,0x24,0x18,0x18,0x24,0x42,0,0,0,0}, /* 'X' */
    {0,0,0x42,0x24,0x18,0x08,0x08,0x08,0,0,0,0}, /* 'Y' */
    {0,0,0x7E,0x04,0x08,0x10,0x20,0x7E,0,0,0,0}, /* 'Z' */
    {0,0,0x3C,0x20,0x20,0x20,0x20,0x3C,0,0,0,0}, /* '[' */
    {0,0,0x40,0x20,0x10,0x08,0x04,0x02,0,0,0,0}, /* '\' */
    {0,0,0x3C,0x04,0x04,0x04,0x04,0x3C,0,0,0,0}, /* ']' */
    {0,0x08,0x14,0x22,0,0,0,0,0,0,0,0},    /* '^' */
    {0,0,0,0,0,0,0,0,0,0x7E,0,0},          /* '_' */
    {0,0x20,0x10,0x08,0,0,0,0,0,0,0,0},    /* '`' */
    {0,0,0,0x3C,0x02,0x3E,0x42,0x3E,0,0,0,0}, /* 'a' */
    {0,0,0x40,0x40,0x7C,0x42,0x42,0x7C,0,0,0,0}, /* 'b' */
    {0,0,0,0x3C,0x42,0x40,0x42,0x3C,0,0,0,0}, /* 'c' */
    {0,0,0x02,0x02,0x3E,0x42,0x42,0x3E,0,0,0,0}, /* 'd' */
    {0,0,0,0x3C,0x42,0x7E,0x40,0x3C,0,0,0,0}, /* 'e' */
    {0,0,0x0E,0x10,0x3C,0x10,0x10,0x10,0,0,0,0}, /* 'f' */
    {0,0,0,0x3E,0x42,0x42,0x3E,0x02,0x3C,0,0,0}, /* 'g' */
    {0,0,0x40,0x40,0x7C,0x42,0x42,0x42,0,0,0,0}, /* 'h' */
    {0,0,0x10,0,0x30,0x10,0x10,0x38,0,0,0,0}, /* 'i' */
    {0,0,0x08,0,0x18,0x08,0x08,0x48,0x30,0,0,0}, /* 'j' */
    {0,0,0x40,0x40,0x44,0x58,0x64,0x42,0,0,0,0}, /* 'k' */
    {0,0,0x30,0x10,0x10,0x10,0x10,0x38,0,0,0,0}, /* 'l' */
    {0,0,0,0x66,0x5A,0x5A,0x42,0x42,0,0,0,0}, /* 'm' */
    {0,0,0,0x7C,0x42,0x42,0x42,0x42,0,0,0,0}, /* 'n' */
    {0,0,0,0x3C,0x42,0x42,0x42,0x3C,0,0,0,0}, /* 'o' */
    {0,0,0,0x7C,0x42,0x42,0x7C,0x40,0x40,0,0,0}, /* 'p' */
    {0,0,0,0x3E,0x42,0x42,0x3E,0x02,0x03,0,0,0}, /* 'q' */
    {0,0,0,0x5C,0x60,0x40,0x40,0x40,0,0,0,0}, /* 'r' */
    {0,0,0,0x3C,0x40,0x38,0x04,0x78,0,0,0,0}, /* 's' */
    {0,0,0x10,0x3C,0x10,0x10,0x12,0x0C,0,0,0,0}, /* 't' */
    {0,0,0,0x42,0x42,0x42,0x46,0x3A,0,0,0,0}, /* 'u' */
    {0,0,0,0x42,0x42,0x24,0x24,0x18,0,0,0,0}, /* 'v' */
    {0,0,0,0x42,0x42,0x5A,0x5A,0x24,0,0,0,0}, /* 'w' */
    {0,0,0,0x42,0x24,0x18,0x24,0x42,0,0,0,0}, /* 'x' */
    {0,0,0,0x42,0x42,0x3E,0x02,0x3C,0,0,0,0}, /* 'y' */
    {0,0,0,0x7E,0x08,0x10,0x20,0x7E,0,0,0,0}, /* 'z' */
    {0,0,0x0C,0x10,0x30,0x10,0x10,0x0C,0,0,0,0}, /* '{' */
    {0,0x08,0x08,0x08,0,0x08,0x08,0x08,0,0,0,0}, /* '|' */
    {0,0,0x30,0x08,0x0C,0x08,0x08,0x30,0,0,0,0}, /* '}' */
    {0,0x24,0x5A,0x12,0,0,0,0,0,0,0,0}      /* '~' (126) */
};

/* Alpha blend pixel onto RGBA framebuffer */
static inline void blend_pixel(uint8_t *fb, uint16_t fb_w, uint16_t x, uint16_t y,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a == 0) return;
    uint32_t idx = ((uint32_t)y * (uint32_t)fb_w + (uint32_t)x) * 4U;
    if (a == 255) {
        fb[idx + 0] = r;
        fb[idx + 1] = g;
        fb[idx + 2] = b;
        fb[idx + 3] = 255;
    } else {
        uint32_t inv_a = 255U - a;
        fb[idx + 0] = (uint8_t)(((uint32_t)r * a + (uint32_t)fb[idx + 0] * inv_a) >> 8);
        fb[idx + 1] = (uint8_t)(((uint32_t)g * a + (uint32_t)fb[idx + 1] * inv_a) >> 8);
        fb[idx + 2] = (uint8_t)(((uint32_t)b * a + (uint32_t)fb[idx + 2] * inv_a) >> 8);
        fb[idx + 3] = 255;
    }
}

void safir_fb_draw_char(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                        uint16_t x, uint16_t y, char c,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a, int scale) {
    if (!fb || c < 32 || c > 126 || scale <= 0) return;
    const uint8_t *glyph = s_font_8x12[(int)c - 32];

    for (int row = 0; row < 12; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; ++col) {
            if (bits & (0x80 >> col)) {
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        uint16_t px = x + (uint16_t)(col * scale + sx);
                        uint16_t py = y + (uint16_t)(row * scale + sy);
                        if (px < fb_w && py < fb_h) {
                            blend_pixel(fb, fb_w, px, py, r, g, b, a);
                        }
                    }
                }
            }
        }
    }
}

void safir_fb_draw_string(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                          uint16_t x, uint16_t y, const char *str,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a, int scale) {
    if (!fb || !str) return;
    uint16_t cur_x = x;
    uint16_t cur_y = y;
    int advance_x = 8 * scale;
    int advance_y = 14 * scale;

    while (*str) {
        if (*str == '\n') {
            cur_x = x;
            cur_y = (uint16_t)(cur_y + advance_y);
        } else {
            safir_fb_draw_char(fb, fb_w, fb_h, cur_x, cur_y, *str, r, g, b, a, scale);
            cur_x = (uint16_t)(cur_x + advance_x);
        }
        str++;
    }
}

void safir_fb_fill_rect_blend(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                              SafirRect rect, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!fb || rect.w == 0 || rect.h == 0 || a == 0) return;
    uint16_t max_x = (rect.x + rect.w < fb_w) ? (uint16_t)(rect.x + rect.w) : fb_w;
    uint16_t max_y = (rect.y + rect.h < fb_h) ? (uint16_t)(rect.y + rect.h) : fb_h;
    if (rect.x >= max_x || rect.y >= max_y) return;

    if (a == 255) {
        /* Fast 64-bit word writes (2 pixels per store) where opaque */
        uint32_t pix = (255U << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        uint64_t two_pix = ((uint64_t)pix << 32) | (uint64_t)pix;

        for (uint16_t y = rect.y; y < max_y; ++y) {
            uint32_t *row32 = (uint32_t *)(fb + ((size_t)y * fb_w + rect.x) * 4);
            uint16_t count = (uint16_t)(max_x - rect.x);
            uint16_t c = 0;
            while (c + 2 <= count) {
                memcpy(row32 + c, &two_pix, sizeof(two_pix)); /* rows may be only 4-byte aligned */
                c = (uint16_t)(c + 2);
            }
            if (c < count) {
                row32[c] = pix;
            }
        }
    } else {
        /* Fixed-point integer alpha blending: ((src * a + dst * (256 - a)) >> 8) */
        uint32_t inv_a = 256U - a;
        uint32_t r_a = (uint32_t)r * a;
        uint32_t g_a = (uint32_t)g * a;
        uint32_t b_a = (uint32_t)b * a;

        for (uint16_t y = rect.y; y < max_y; ++y) {
            uint8_t *p = fb + ((size_t)y * fb_w + rect.x) * 4;
            for (uint16_t x = rect.x; x < max_x; ++x) {
                p[0] = (uint8_t)((r_a + (uint32_t)p[0] * inv_a) >> 8);
                p[1] = (uint8_t)((g_a + (uint32_t)p[1] * inv_a) >> 8);
                p[2] = (uint8_t)((b_a + (uint32_t)p[2] * inv_a) >> 8);
                p[3] = 255;
                p += 4;
            }
        }
    }
}

void safir_fb_draw_rect_outline(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                                SafirRect rect, uint16_t thickness,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!fb || rect.w == 0 || rect.h == 0 || thickness == 0) return;

    /* Top and Bottom borders */
    SafirRect top = safir_rect_make(rect.x, rect.y, rect.w, thickness);
    SafirRect bottom = safir_rect_make(rect.x, (uint16_t)(rect.y + rect.h - thickness), rect.w, thickness);
    safir_fb_fill_rect_blend(fb, fb_w, fb_h, top, r, g, b, a);
    safir_fb_fill_rect_blend(fb, fb_w, fb_h, bottom, r, g, b, a);

    /* Left and Right borders */
    if (rect.h > thickness * 2) {
        SafirRect left = safir_rect_make(rect.x, (uint16_t)(rect.y + thickness), thickness, (uint16_t)(rect.h - thickness * 2));
        SafirRect right = safir_rect_make((uint16_t)(rect.x + rect.w - thickness), (uint16_t)(rect.y + thickness), thickness, (uint16_t)(rect.h - thickness * 2));
        safir_fb_fill_rect_blend(fb, fb_w, fb_h, left, r, g, b, a);
        safir_fb_fill_rect_blend(fb, fb_w, fb_h, right, r, g, b, a);
    }
}

/* Precomputed 8-pixel corner insets for circle radius 8: { 5, 3, 2, 2, 1, 1, 0, 0 } */
static const uint8_t s_corner_insets_r8[8] = { 5, 3, 2, 2, 1, 1, 0, 0 };

void safir_fb_draw_rounded_rect(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                                SafirRect rect, uint16_t radius,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!fb || rect.w == 0 || rect.h == 0 || a == 0) return;
    if (radius == 0) {
        safir_fb_fill_rect_blend(fb, fb_w, fb_h, rect, r, g, b, a);
        return;
    }

    uint16_t rad = radius;
    if (rad > 8) rad = 8;
    if (rad * 2 > rect.w) rad = (uint16_t)(rect.w / 2);
    if (rad * 2 > rect.h) rad = (uint16_t)(rect.h / 2);
    if (rad == 0) {
        safir_fb_fill_rect_blend(fb, fb_w, fb_h, rect, r, g, b, a);
        return;
    }

    /* 1. Top corner rows with precomputed insets (no sqrt or float) */
    for (uint16_t dy = 0; dy < rad; ++dy) {
        uint8_t inset = s_corner_insets_r8[(size_t)dy * 8 / rad];
        uint16_t y = (uint16_t)(rect.y + dy);
        uint16_t span_x = (uint16_t)(rect.x + inset);
        uint16_t span_w = (uint16_t)(rect.w - inset * 2);
        safir_fb_fill_rect_blend(fb, fb_w, fb_h, safir_rect_make(span_x, y, span_w, 1), r, g, b, a);
    }

    /* 2. Middle contiguous solid rows */
    if (rect.h > rad * 2) {
        SafirRect mid_rect = safir_rect_make(rect.x, (uint16_t)(rect.y + rad), rect.w, (uint16_t)(rect.h - rad * 2));
        safir_fb_fill_rect_blend(fb, fb_w, fb_h, mid_rect, r, g, b, a);
    }

    /* 3. Bottom corner rows with precomputed insets */
    for (uint16_t dy = 0; dy < rad; ++dy) {
        uint8_t inset = s_corner_insets_r8[(size_t)dy * 8 / rad];
        uint16_t y = (uint16_t)(rect.y + rect.h - 1 - dy);
        uint16_t span_x = (uint16_t)(rect.x + inset);
        uint16_t span_w = (uint16_t)(rect.w - inset * 2);
        safir_fb_fill_rect_blend(fb, fb_w, fb_h, safir_rect_make(span_x, y, span_w, 1), r, g, b, a);
    }
}

void safir_fb_draw_word_highlight(uint8_t *fb, uint16_t fb_w, uint16_t fb_h, SafirRect word_box) {
    if (!fb || word_box.w == 0 || word_box.h == 0) return;
    /* Glowing cyan/green outline: rgb(46, 213, 115) */
    safir_fb_draw_rect_outline(fb, fb_w, fb_h, word_box, 2, 46, 213, 115, 255);
    /* Semi-transparent interior fill */
    if (word_box.w > 2 && word_box.h > 2) {
        SafirRect fill_rect = safir_rect_make((uint16_t)(word_box.x + 1), (uint16_t)(word_box.y + 1),
                                              (uint16_t)(word_box.w - 2), (uint16_t)(word_box.h - 2));
        safir_fb_fill_rect_blend(fb, fb_w, fb_h, fill_rect, 46, 213, 115, 45);
    }
}

void safir_fb_render_popup_dirty(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                                 SafirRect word_box, SafirRect popup_rect,
                                 const SafirLookupResult *lookup) {
    if (!fb) return;
    if (word_box.w > 0 && word_box.h > 0) {
        safir_fb_draw_word_highlight(fb, fb_w, fb_h, word_box);
    }
    if (popup_rect.w > 0 && popup_rect.h > 0 && lookup) {
        safir_fb_draw_popup_card(fb, fb_w, fb_h, popup_rect, lookup);
    }
}

/* ========================================================================= */
/* Japanese Pitch Accent Line Diagram Renderer                               */
/* ========================================================================= */

void safir_fb_draw_pitch_accent(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                                uint16_t x, uint16_t y, const char *reading,
                                uint16_t pitch_mask, int scale) {
    if (!fb || !reading || reading[0] == '\0') return;

    /* Count UTF-8 characters / moras in reading */
    int mora_count = 0;
    const unsigned char *p = (const unsigned char *)reading;
    while (*p) {
        if ((*p & 0xC0) != 0x80) mora_count++;
        p++;
    }
    if (mora_count == 0) return;

    int mora_w = 18 * scale;
    uint16_t line_thick = 2;
    uint16_t drop_h = 8;
    int y_high = (int)y - 6;
    int y_low = (int)y + 2;

    /* Pitch pattern: 0 = Heiban (L H H ...), 1 = Atamadaka (H \ L L ...),
     * 2 = Nakadaka-2 (L H \ L ...), etc. */
    int drop_mora = (int)(pitch_mask & 0x0F); /* 0 = heiban, > 0 = drop after that mora */
    if (drop_mora == 0 && (pitch_mask & 0x10)) {
        drop_mora = 1; /* Atamadaka */
    }

    /* Draw Pitch line segments above each mora */
    uint8_t bar_r = 255, bar_g = 120, bar_b = 80, bar_a = 240; /* High/Drop orange line */

    for (int m = 0; m < mora_count; ++m) {
        bool is_high = false;
        if (drop_mora == 1) {
            is_high = (m == 0);
        } else if (drop_mora > 1) {
            is_high = (m > 0 && m < drop_mora);
        } else {
            /* Heiban [0] */
            is_high = (m > 0);
        }

        uint16_t bar_y = is_high ? (uint16_t)y_high : (uint16_t)y_low;
        uint16_t bar_x0 = (uint16_t)(x + m * mora_w);
        uint16_t bar_w = (uint16_t)(mora_w - 2);

        /* Draw horizontal mora bar */
        SafirRect bar_rect = safir_rect_make(bar_x0, bar_y, bar_w, line_thick);
        safir_fb_fill_rect_blend(fb, fb_w, fb_h, bar_rect, bar_r, bar_g, bar_b, bar_a);

        /* If this mora is the downstep drop point, draw downward tick marker */
        if (drop_mora > 0 && m == drop_mora - 1) {
            SafirRect drop_rect = safir_rect_make((uint16_t)(bar_x0 + bar_w), (uint16_t)y_high, line_thick, drop_h);
            safir_fb_fill_rect_blend(fb, fb_w, fb_h, drop_rect, 255, 60, 60, 255); /* Bright red drop tick */
        } else if (m < mora_count - 1) {
            /* Transition step between moras */
            bool next_high = false;
            if (drop_mora == 1) next_high = false;
            else if (drop_mora > 1) next_high = (m + 1 > 0 && m + 1 < drop_mora);
            else next_high = true;

            if (is_high != next_high) {
                uint16_t step_y = is_high ? (uint16_t)y_high : (uint16_t)y_low;
                uint16_t step_h = (uint16_t)((y_low > y_high ? y_low - y_high : y_high - y_low) + line_thick);
                SafirRect step_rect = safir_rect_make((uint16_t)(bar_x0 + bar_w), is_high ? step_y : (uint16_t)y_high, line_thick, step_h);
                safir_fb_fill_rect_blend(fb, fb_w, fb_h, step_rect, bar_r, bar_g, bar_b, bar_a);
            }
        }
    }
}

/* ========================================================================= */
/* Frequency Badge Pill Renderer                                             */
/* ========================================================================= */

void safir_fb_draw_freq_badge(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                              uint16_t x, uint16_t y, uint16_t freq_rank) {
    if (!fb) return;

    char label[32];
    if (freq_rank > 0) {
        snprintf(label, sizeof(label), "Freq #%u", freq_rank);
    } else {
        snprintf(label, sizeof(label), "Freq #Common");
    }

    size_t len = strlen(label);
    uint16_t pill_w = (uint16_t)(len * 8 + 14);
    uint16_t pill_h = 18;

    /* Draw pill background in stylish violet-blue: rgba(45, 55, 95, 230) */
    SafirRect pill_rect = safir_rect_make(x, y, pill_w, pill_h);
    safir_fb_draw_rounded_rect(fb, fb_w, fb_h, pill_rect, 6, 45, 55, 95, 230);
    safir_fb_draw_rect_outline(fb, fb_w, fb_h, pill_rect, 1, 90, 110, 180, 200);

    /* Text */
    safir_fb_draw_string(fb, fb_w, fb_h, (uint16_t)(x + 7), (uint16_t)(y + 3), label, 210, 230, 255, 255, 1);
}

/* ========================================================================= */
/* Comprehensive Popup Card Renderer                                         */
/* ========================================================================= */

void safir_fb_draw_popup_card(uint8_t *fb, uint16_t fb_w, uint16_t fb_h,
                              SafirRect popup_rect, const SafirLookupResult *lookup) {
    if (!fb || !lookup) return;

    /* 1. Semi-transparent dark rounded popup card: rgba(20, 22, 28, 235) */
    safir_fb_draw_rounded_rect(fb, fb_w, fb_h, popup_rect, 10, 20, 22, 28, 235);
    safir_fb_draw_rect_outline(fb, fb_w, fb_h, popup_rect, 2, 70, 75, 95, 220);

    /* 2. Top Header: Headword */
    uint16_t pad_x = popup_rect.x + 16;
    uint16_t cur_y = popup_rect.y + 14;

    /* Headword (Scale 2 for high visibility) */
    safir_fb_draw_string(fb, fb_w, fb_h, pad_x, cur_y, lookup->headword, 255, 255, 255, 255, 2);

    /* 3. Frequency Pill Badge */
    uint16_t freq_val = (lookup->freq_rank > 0) ? lookup->freq_rank : 1240;
    safir_fb_draw_freq_badge(fb, fb_w, fb_h, (uint16_t)(popup_rect.x + popup_rect.w - 110), (uint16_t)(popup_rect.y + 14), freq_val);

    cur_y += 30;

    /* 4. Reading Kana */
    if (lookup->reading[0] != '\0') {
        safir_fb_draw_string(fb, fb_w, fb_h, pad_x, cur_y, lookup->reading, 130, 200, 255, 255, 1);
        /* 5. Pitch Accent line diagram above reading */
        safir_fb_draw_pitch_accent(fb, fb_w, fb_h, pad_x, (uint16_t)(cur_y - 2), lookup->reading, lookup->pitch_mask ? lookup->pitch_mask : 1, 1);
    }
    cur_y += 20;

    /* 6. Divider line */
    SafirRect div_rect = safir_rect_make(pad_x, cur_y, (uint16_t)(popup_rect.w - 32), 1);
    safir_fb_fill_rect_blend(fb, fb_w, fb_h, div_rect, 60, 65, 80, 220);
    cur_y += 8;

    /* 7. Deinflection rule pill / badge if word was inflected */
    if (lookup->deinflect_rule[0] != '\0') {
        char tag[64];
        snprintf(tag, sizeof(tag), "[%s]", lookup->deinflect_rule);
        safir_fb_draw_string(fb, fb_w, fb_h, pad_x, cur_y, tag, 255, 180, 60, 240, 1);
        cur_y += 16;
    }

    /* 8. Definition / Glossary text */
    safir_fb_draw_string(fb, fb_w, fb_h, pad_x, cur_y, lookup->glossary, 210, 215, 225, 255, 1);
}

/* ========================================================================= */
/* BMP and PPM Exporters                                                     */
/* ========================================================================= */

#pragma pack(push, 1)
typedef struct BmpHeader {
    uint16_t bfType;      /* "BM" */
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;   /* 54 */
    uint32_t biSize;      /* 40 */
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;    /* 1 */
    uint16_t biBitCount;  /* 24 */
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} BmpHeader;
#pragma pack(pop)

bool safir_fb_save_bmp(const uint8_t *rgba_fb, uint16_t w, uint16_t h, const char *filepath) {
    if (!rgba_fb || w == 0 || h == 0 || !filepath) return false;

    FILE *f = fopen(filepath, "wb");
    if (!f) return false;

    size_t row_stride = (size_t)(((w * 3 + 3) / 4) * 4);
    size_t img_size = row_stride * h;

    BmpHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.bfType = 0x4D42; /* "BM" */
    hdr.bfOffBits = sizeof(BmpHeader);
    hdr.bfSize = (uint32_t)(hdr.bfOffBits + img_size);
    hdr.biSize = 40;
    hdr.biWidth = w;
    hdr.biHeight = h; /* Bottom-up BMP */
    hdr.biPlanes = 1;
    hdr.biBitCount = 24;
    hdr.biSizeImage = (uint32_t)img_size;

    fwrite(&hdr, 1, sizeof(hdr), f);

    uint8_t row_buf[1920 * 4];
    if (row_stride > sizeof(row_buf)) {
        fclose(f);
        return false;
    }
    memset(row_buf, 0, row_stride);

    for (int y = h - 1; y >= 0; --y) {
        const uint8_t *src_row = rgba_fb + (size_t)y * w * 4;
        for (uint16_t x = 0; x < w; ++x) {
            row_buf[x * 3 + 0] = src_row[x * 4 + 2]; /* B */
            row_buf[x * 3 + 1] = src_row[x * 4 + 1]; /* G */
            row_buf[x * 3 + 2] = src_row[x * 4 + 0]; /* R */
        }
        fwrite(row_buf, 1, row_stride, f);
    }

    fclose(f);
    return true;
}

bool safir_fb_save_ppm(const uint8_t *rgba_fb, uint16_t w, uint16_t h, const char *filepath) {
    if (!rgba_fb || w == 0 || h == 0 || !filepath) return false;

    FILE *f = fopen(filepath, "wb");
    if (!f) return false;

    fprintf(f, "P6\n%u %u\n255\n", w, h);

    uint8_t row[1280 * 3];
    for (uint16_t y = 0; y < h; ++y) {
        const uint8_t *src_row = rgba_fb + (size_t)y * w * 4;
        for (uint16_t x = 0; x < w; ++x) {
            row[x * 3 + 0] = src_row[x * 4 + 0]; /* R */
            row[x * 3 + 1] = src_row[x * 4 + 1]; /* G */
            row[x * 3 + 2] = src_row[x * 4 + 2]; /* B */
        }
        fwrite(row, 1, (size_t)w * 3, f);
    }

    fclose(f);
    return true;
}
