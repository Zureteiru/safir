#ifndef SAFIR_TYPES_H
#define SAFIR_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAFIR_SCREEN_WIDTH        1280
#define SAFIR_SCREEN_HEIGHT        720

/* Default VN dialogue region on 1280x720 Nintendo Switch screen */
#define SAFIR_DEFAULT_DIALOGUE_X    90
#define SAFIR_DEFAULT_DIALOGUE_Y   480
#define SAFIR_DEFAULT_DIALOGUE_W  1100
#define SAFIR_DEFAULT_DIALOGUE_H   210

#define SAFIR_MAX_CHARS_PER_LINE   128
#define SAFIR_MAX_LINES_PER_BOX      8

typedef struct SafirRect {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} SafirRect;

static inline SafirRect safir_rect_make(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    SafirRect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

static inline SafirRect safir_default_dialogue_rect(void) {
    return safir_rect_make(
        SAFIR_DEFAULT_DIALOGUE_X,
        SAFIR_DEFAULT_DIALOGUE_Y,
        SAFIR_DEFAULT_DIALOGUE_W,
        SAFIR_DEFAULT_DIALOGUE_H
    );
}

typedef struct SafirCharBox {
    uint32_t codepoint;   /* UTF-32 / Unicode codepoint */
    SafirRect screen_box; /* Screen coordinates (0..1280, 0..720) */
    float confidence;     /* Recognition confidence [0.0f .. 1.0f] */
} SafirCharBox;

typedef struct SafirTextLine {
    SafirCharBox chars[SAFIR_MAX_CHARS_PER_LINE];
    uint16_t char_count;
    uint64_t dhash;
} SafirTextLine;

typedef struct SafirOcrResult {
    SafirTextLine lines[SAFIR_MAX_LINES_PER_BOX];
    uint16_t line_count;
    uint64_t dhash;
    bool cached;          /* True if reused from 0ms perceptual hash cache */
} SafirOcrResult;

#ifdef __cplusplus
}
#endif

#endif /* SAFIR_TYPES_H */
