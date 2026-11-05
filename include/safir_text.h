#ifndef SAFIR_TEXT_H
#define SAFIR_TEXT_H

#include <stddef.h>
#include "safir_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Writes a recognized line as null-terminated UTF-8. Stops at a whole character when
 * max_buf is reached. Returns the number of bytes written (without the terminator).
 */
size_t safir_text_line_to_utf8(const SafirTextLine *line, char *out_buf, size_t max_buf);

#ifdef __cplusplus
}
#endif

#endif /* SAFIR_TEXT_H */
