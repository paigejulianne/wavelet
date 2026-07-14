/* png.h -- minimal dependency-free PNG decoder for the wavelet GUI.
 *
 * Decodes 8-bit non-interlaced PNGs (grayscale, RGB, palette, and their
 * alpha variants) into an Image. Alpha is composited over white. Returns 1 on
 * success, 0 on failure (unsupported subformat or malformed data). */
#ifndef PNG_DECODE_H
#define PNG_DECODE_H

#include <stddef.h>
#include <stdint.h>
#include "image_io.h"

int png_decode(const uint8_t *data, size_t n, Image *out);

#endif
