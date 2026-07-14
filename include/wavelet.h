/*
 * wavelet.h -- public C API for the wavelet file/image compressor.
 *
 * Cross-platform shared library:
 *   - Windows : wavelet.dll  (+ wavelet.lib import library)
 *   - Linux   : libwavelet.so
 *   - macOS   : libwavelet.dylib
 *
 * Pipeline:  data -> pad -> multi-level DWT (1D or separable 2D)
 *            -> quantize -> zigzag/varint -> order-0 range coder -> container
 *
 * All functions are C-linkage and safe to call from C or C++.
 * Buffers returned via out-parameters are heap-allocated by the library and
 * must be released with wv_free().
 */
#ifndef WAVELET_H
#define WAVELET_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(WV_BUILD_SHARED)
#    define WV_API __declspec(dllexport)
#  elif defined(WV_STATIC)
#    define WV_API
#  else
#    define WV_API __declspec(dllimport)
#  endif
#else
#  if defined(WV_BUILD_SHARED)
#    define WV_API __attribute__((visibility("default")))
#  else
#    define WV_API
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Wavelet choice. */
enum wv_wavelet {
    WV_WAVELET_AUTO = -1,
    WV_WAVELET_53   = 0,   /* Le Gall 5/3, integer, reversible (lossless)   */
    WV_WAVELET_97   = 1    /* CDF 9/7, float, better compaction (lossy)     */
};

/* Container mode, as reported by wv_inspect(). */
enum wv_mode {
    WV_MODE_STORED_RAW = 0, /* payload stored verbatim (never-expand guard) */
    WV_MODE_LOSSLESS   = 1, /* 5/3 transform, exact reconstruction          */
    WV_MODE_LOSSY      = 2, /* 9/7 transform, quantized                     */
    WV_MODE_STORED_RC  = 3  /* no transform, range-coded raw bytes          */
};

/* Error codes (return values). */
enum wv_status {
    WV_OK               =  0,
    WV_ERR_ARG          = -1,  /* bad argument                              */
    WV_ERR_MAGIC        = -2,  /* not a WVLC container                      */
    WV_ERR_VERSION      = -3,  /* unsupported container version             */
    WV_ERR_CORRUPT      = -4,  /* truncated or malformed container          */
    WV_ERR_CRC          = -5,  /* CRC mismatch (lossless integrity failure) */
    WV_ERR_OOM          = -6   /* allocation failure                        */
};

/* Metadata describing a container, filled by wv_inspect(). */
typedef struct wv_info {
    int      version;
    int      mode;        /* enum wv_mode    */
    int      wavelet;     /* enum wv_wavelet */
    int      levels;
    int      ndim;        /* 1 = byte stream, 2 = image        */
    int      channels;    /* 1 (gray) or 3 (RGB) for 2D        */
    uint32_t width;       /* 2D only                           */
    uint32_t height;      /* 2D only                           */
    uint64_t original_len;/* raw byte count of the source data */
    double   quant_step;  /* lossy only                        */
    uint32_t crc32;
} wv_info;

/* Library version string, e.g. "1.0.0". */
WV_API const char *wv_version(void);

/* Human-readable message for a wv_status code. */
WV_API const char *wv_strerror(int status);

/* Free a buffer returned by this library. NULL is ignored. */
WV_API void wv_free(void *ptr);

/*
 * Compress a 1D byte stream.
 *   lossy    : 0 = lossless (5/3), 1 = lossy (9/7)
 *   wavelet  : WV_WAVELET_AUTO or an explicit wv_wavelet
 *   levels   : decomposition depth, or <=0 for automatic
 *   quality  : 1..100 for lossy (higher = better fidelity), <=0 = default 75
 * On success *out receives a malloc'd container; free with wv_free().
 */
WV_API int wv_compress(const uint8_t *data, size_t len,
                       int lossy, int wavelet, int levels, int quality,
                       uint8_t **out, size_t *out_len);

/*
 * Compress an image using the separable 2D DWT.
 *   pixels   : row-major, channel-interleaved (gray: 1, RGB: 3 bytes/pixel)
 *   width,height,channels : image geometry
 * Other parameters and ownership match wv_compress().
 */
WV_API int wv_compress_image(const uint8_t *pixels,
                             uint32_t width, uint32_t height, int channels,
                             int lossy, int wavelet, int levels, int quality,
                             uint8_t **out, size_t *out_len);

/*
 * Decompress any container (1D or 2D). Returns the reconstructed raw bytes:
 * for a 2D image that is width*height*channels bytes, row-major interleaved.
 * Use wv_inspect() first if you need the geometry. Free *out with wv_free().
 */
WV_API int wv_decompress(const uint8_t *blob, size_t len,
                         uint8_t **out, size_t *out_len);

/* Read container metadata without decompressing. */
WV_API int wv_inspect(const uint8_t *blob, size_t len, wv_info *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WAVELET_H */
