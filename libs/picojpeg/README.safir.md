# Vendored: picojpeg v1.1 (public domain)

`picojpeg.c` / `picojpeg.h` copied from https://github.com/richgel999/picojpeg.
Public domain (Rich Geldreich). Baseline JPEG, decodes one MCU (<= 16x16 px) at a time
with ~2.4 KB of static state, so the overlay can decode only the dialogue region of a
720p screenshot without a full-frame buffer.


## Local change

`convertCb()` / `convertCr()` wrote `*pDst++ = addAndClamp(pDst[0], v)`, which reads
and increments `pDst` unsequenced (undefined behaviour; GCC `-Wsequence-point`). Those
four statements are rewritten as `pDst[0] = addAndClamp(pDst[0], v); pDst++;`, which is
what the original intended. `getExtendOffset()` returned `((-1) << n) + 1` (left shift of a negative value, undefined
behaviour flagged by UBSan); it now returns the identical value `1 - (1 << n)`.
Nothing else is changed.

Built with `-Wno-unused-but-set-variable -Wno-unused-function` (unused progressive
JPEG fields and an unused `findEOI`).
