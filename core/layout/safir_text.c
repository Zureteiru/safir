#include "safir_text.h"

static size_t codepoint_to_utf8(uint32_t cp, char *out) {
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

size_t safir_text_line_to_utf8(const SafirTextLine *line, char *out_buf, size_t max_buf) {
    if (!line || !out_buf || max_buf == 0) return 0;
    size_t out_idx = 0;
    for (uint16_t i = 0; i < line->char_count; ++i) {
        char buf[4];
        size_t n = codepoint_to_utf8(line->chars[i].codepoint, buf);
        if (out_idx + n >= max_buf) break;
        for (size_t b = 0; b < n; ++b) out_buf[out_idx++] = buf[b];
    }
    out_buf[out_idx] = '\0';
    return out_idx;
}
