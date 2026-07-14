/*
 * cli.c -- command-line front-end for the wavelet compressor library.
 *
 *   wavelet compress   INPUT OUTPUT [--lossy] [--quality N]
 *                                   [--wavelet 53|97] [--levels N] [--raw]
 *   wavelet decompress INPUT OUTPUT
 *   wavelet info       FILE
 *
 * Binary PGM (P5, grayscale) and PPM (P6, RGB) inputs are auto-detected and
 * compressed with the separable 2D transform; everything else is treated as a
 * 1D byte stream. Use --raw to force 1D even for netpbm inputs.
 */
#include "wavelet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, uint8_t **data, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open '%s'\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return 0; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *data = buf; *len = got;
    return 1;
}

static int write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "error: cannot write '%s'\n", path); return 0; }
    size_t put = fwrite(data, 1, len, f);
    fclose(f);
    return put == len;
}

/* --- minimal binary netpbm (P5/P6) parsing --- */

static int skip_ws_comments(const uint8_t *p, size_t n, size_t *pos) {
    for (;;) {
        while (*pos < n && (p[*pos]==' '||p[*pos]=='\t'||p[*pos]=='\n'||p[*pos]=='\r')) (*pos)++;
        if (*pos < n && p[*pos]=='#') { while (*pos < n && p[*pos] != '\n') (*pos)++; }
        else break;
    }
    return *pos < n;
}
static int read_uint(const uint8_t *p, size_t n, size_t *pos, uint32_t *val) {
    if (!skip_ws_comments(p, n, pos)) return 0;
    if (*pos >= n || p[*pos] < '0' || p[*pos] > '9') return 0;
    uint32_t v = 0;
    while (*pos < n && p[*pos] >= '0' && p[*pos] <= '9') v = v*10 + (uint32_t)(p[(*pos)++]-'0');
    *val = v;
    return 1;
}

/* Returns 1 and fills geometry+pixel pointer if data is a valid P5/P6 image. */
static int parse_netpbm(const uint8_t *data, size_t len,
                        uint32_t *w, uint32_t *h, int *channels,
                        const uint8_t **pixels) {
    if (len < 2 || data[0] != 'P' || (data[1] != '5' && data[1] != '6')) return 0;
    *channels = (data[1] == '5') ? 1 : 3;
    size_t pos = 2;
    uint32_t maxval;
    if (!read_uint(data, len, &pos, w)) return 0;
    if (!read_uint(data, len, &pos, h)) return 0;
    if (!read_uint(data, len, &pos, &maxval)) return 0;
    if (maxval != 255) return 0;                 /* only 8-bit samples */
    pos++;                                        /* single whitespace after maxval */
    size_t need = (size_t)(*w) * (*h) * (*channels);
    if (pos + need > len) return 0;
    *pixels = data + pos;
    return 1;
}

static int write_netpbm(const char *path, const uint8_t *pixels,
                        uint32_t w, uint32_t h, int channels) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "error: cannot write '%s'\n", path); return 0; }
    fprintf(f, "P%c\n%u %u\n255\n", channels == 1 ? '5' : '6', w, h);
    size_t need = (size_t)w * h * channels;
    size_t put = fwrite(pixels, 1, need, f);
    fclose(f);
    return put == need;
}

static const char *mode_name(int m) {
    switch (m) {
        case WV_MODE_STORED_RAW: return "stored (verbatim)";
        case WV_MODE_LOSSLESS:   return "lossless 5/3";
        case WV_MODE_LOSSY:      return "lossy 9/7";
        case WV_MODE_STORED_RC:  return "stored (range-coded)";
        default:                 return "?";
    }
}

static int cmd_compress(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: compress INPUT OUTPUT [options]\n"); return 2; }
    const char *in = argv[0], *out = argv[1];
    int lossy = 0, wavelet = WV_WAVELET_AUTO, levels = -1, quality = -1, force_raw = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--lossy")) lossy = 1;
        else if (!strcmp(argv[i], "--raw")) force_raw = 1;
        else if (!strcmp(argv[i], "--quality") && i+1 < argc) { quality = atoi(argv[++i]); lossy = 1; }
        else if (!strcmp(argv[i], "--levels")  && i+1 < argc) levels = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wavelet") && i+1 < argc) {
            wavelet = !strcmp(argv[i+1], "97") ? WV_WAVELET_97 : WV_WAVELET_53; i++;
        } else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
    }

    uint8_t *data; size_t len;
    if (!read_file(in, &data, &len)) return 1;

    uint8_t *blob = NULL; size_t blob_len = 0;
    uint32_t w = 0, h = 0; int ch = 0; const uint8_t *px = NULL;
    int is_image = !force_raw && parse_netpbm(data, len, &w, &h, &ch, &px);

    int rc;
    if (is_image)
        rc = wv_compress_image(px, w, h, ch, lossy, wavelet, levels, quality, &blob, &blob_len);
    else
        rc = wv_compress(data, len, lossy, wavelet, levels, quality, &blob, &blob_len);

    if (rc != WV_OK) {
        fprintf(stderr, "compress failed: %s\n", wv_strerror(rc));
        free(data); return 1;
    }
    if (!write_file(out, blob, blob_len)) { free(data); wv_free(blob); return 1; }

    wv_info info; wv_inspect(blob, blob_len, &info);
    double ratio = len ? (double)blob_len / (double)len : 0.0;
    if (is_image)
        printf("compressed %ux%u image (%d ch, %zu B) -> %zu B (%.2f%%, %s)\n",
               w, h, ch, len, blob_len, ratio * 100.0, mode_name(info.mode));
    else
        printf("compressed %zu -> %zu B (%.2f%%, %s)\n",
               len, blob_len, ratio * 100.0, mode_name(info.mode));

    free(data); wv_free(blob);
    return 0;
}

static int cmd_decompress(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: decompress INPUT OUTPUT\n"); return 2; }
    uint8_t *blob; size_t blob_len;
    if (!read_file(argv[0], &blob, &blob_len)) return 1;

    wv_info info;
    int rc = wv_inspect(blob, blob_len, &info);
    if (rc != WV_OK) { fprintf(stderr, "inspect failed: %s\n", wv_strerror(rc)); free(blob); return 1; }

    uint8_t *data; size_t len;
    rc = wv_decompress(blob, blob_len, &data, &len);
    if (rc != WV_OK) { fprintf(stderr, "decompress failed: %s\n", wv_strerror(rc)); free(blob); return 1; }

    int ok;
    if (info.ndim == 2)
        ok = write_netpbm(argv[1], data, info.width, info.height, info.channels);
    else
        ok = write_file(argv[1], data, len);

    if (ok) printf("decompressed -> %zu B\n", len);
    free(blob); wv_free(data);
    return ok ? 0 : 1;
}

static int cmd_info(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: info FILE\n"); return 2; }
    uint8_t *blob; size_t blob_len;
    if (!read_file(argv[0], &blob, &blob_len)) return 1;
    wv_info h;
    int rc = wv_inspect(blob, blob_len, &h);
    if (rc != WV_OK) { fprintf(stderr, "info failed: %s\n", wv_strerror(rc)); free(blob); return 1; }
    printf("file            %s\n", argv[0]);
    printf("container size  %zu bytes\n", blob_len);
    printf("format version  %d\n", h.version);
    printf("mode            %s\n", mode_name(h.mode));
    printf("wavelet         %s\n", h.wavelet == WV_WAVELET_97 ? "9/7" : "5/3");
    printf("levels          %d\n", h.levels);
    printf("dimensions      %s\n", h.ndim == 2 ? "2D image" : "1D stream");
    if (h.ndim == 2)
        printf("geometry        %ux%u, %d channel(s)\n", h.width, h.height, h.channels);
    printf("original length %llu bytes\n", (unsigned long long)h.original_len);
    if (h.mode == WV_MODE_LOSSY)
        printf("quant step      %g\n", h.quant_step);
    printf("crc32           0x%08x\n", h.crc32);
    free(blob);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "wavelet compressor %s\n"
            "usage:\n"
            "  %s compress   INPUT OUTPUT [--lossy] [--quality N]\n"
            "                             [--wavelet 53|97] [--levels N] [--raw]\n"
            "  %s decompress INPUT OUTPUT\n"
            "  %s info       FILE\n",
            wv_version(), argv[0], argv[0], argv[0]);
        return 2;
    }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "compress"))   return cmd_compress(argc - 2, argv + 2);
    if (!strcmp(cmd, "decompress")) return cmd_decompress(argc - 2, argv + 2);
    if (!strcmp(cmd, "info"))       return cmd_info(argc - 2, argv + 2);
    fprintf(stderr, "unknown command: %s\n", cmd);
    return 2;
}
