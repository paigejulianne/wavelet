/* image_io.h -- tiny image loader/saver for the wavelet GUI.
 *
 * Supports uncompressed BMP (24-bit and 8-bit palette) and binary netpbm
 * (P5 grayscale / P6 RGB). Pixels are stored row-major, channel-interleaved,
 * 8 bits per sample: channels == 1 (gray) or 3 (RGB).
 */
#ifndef IMAGE_IO_H
#define IMAGE_IO_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int      w;
    int      h;
    int      channels;   /* 1 or 3 */
    uint8_t *px;         /* w*h*channels bytes, or NULL if empty */
} Image;

/* Load by file extension / magic. Returns 1 on success, 0 on failure. */
int  img_load(const char *path, Image *out);

/* Save by output extension (.bmp / .pgm / .ppm). Returns 1 on success. */
int  img_save(const char *path, const Image *im);

/* Release pixels and zero the struct. */
void img_free(Image *im);

/* Deep copy src into dst (dst must be freed by caller). Returns 1/0. */
int  img_copy(Image *dst, const Image *src);

/* Build a top-down, tightly packed BGR-24 buffer for GDI StretchDIBits.
 * Returns a malloc'd buffer of w*h*3 bytes (caller frees) or NULL. */
uint8_t *img_to_bgr24(const Image *im);

#endif /* IMAGE_IO_H */
