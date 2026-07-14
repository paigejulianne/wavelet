/* test.c -- in-process correctness tests for the wavelet library. */
#include "wavelet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); failures++; } } while (0)

static double rmse(const uint8_t *a, const uint8_t *b, size_t n) {
    double s = 0; for (size_t i = 0; i < n; i++) { double d = (double)a[i]-b[i]; s += d*d; }
    return sqrt(s / (double)n);
}

static void test_1d_lossless(void) {
    printf("[1D lossless]\n");
    size_t n = 40000;
    uint8_t *data = malloc(n);
    for (size_t i = 0; i < n; i++) data[i] = (uint8_t)(128 + 100*sin(i/40.0));
    uint8_t *blob, *out; size_t bl, ol;
    CHECK(wv_compress(data, n, 0, WV_WAVELET_AUTO, -1, -1, &blob, &bl) == WV_OK, "compress ok");
    CHECK(wv_decompress(blob, bl, &out, &ol) == WV_OK, "decompress ok");
    CHECK(ol == n && memcmp(data, out, n) == 0, "bit-exact roundtrip");
    CHECK(bl < n, "smaller than input");
    printf("  %zu -> %zu bytes\n", n, bl);
    wv_free(blob); wv_free(out); free(data);
}

static void test_1d_lossy(void) {
    printf("[1D lossy]\n");
    size_t n = 40000;
    uint8_t *data = malloc(n);
    for (size_t i = 0; i < n; i++) data[i] = (uint8_t)(128 + 100*sin(i/40.0));
    uint8_t *blob, *out; size_t bl, ol;
    CHECK(wv_compress(data, n, 1, WV_WAVELET_AUTO, -1, 90, &blob, &bl) == WV_OK, "compress ok");
    CHECK(wv_decompress(blob, bl, &out, &ol) == WV_OK, "decompress ok");
    CHECK(ol == n, "length preserved");
    double e = rmse(data, out, n);
    CHECK(e < 5.0, "rmse < 5");
    printf("  %zu -> %zu bytes, rmse=%.3f\n", n, bl, e);
    wv_free(blob); wv_free(out); free(data);
}

static void test_1d_random(void) {
    printf("[1D incompressible]\n");
    size_t n = 8000;
    uint8_t *data = malloc(n);
    unsigned s = 12345;
    for (size_t i = 0; i < n; i++) { s = s*1103515245u + 12345u; data[i] = (uint8_t)(s >> 16); }
    uint8_t *blob, *out; size_t bl, ol;
    wv_compress(data, n, 0, WV_WAVELET_AUTO, -1, -1, &blob, &bl);
    wv_decompress(blob, bl, &out, &ol);
    CHECK(ol == n && memcmp(data, out, n) == 0, "bit-exact roundtrip");
    CHECK(bl <= n + 64, "no meaningful expansion (<= header)");
    wv_free(blob); wv_free(out); free(data);
}

static void test_2d_lossless(void) {
    printf("[2D lossless image]\n");
    uint32_t w = 256, h = 200; int ch = 3;
    size_t n = (size_t)w*h*ch;
    uint8_t *px = malloc(n);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t o = ((size_t)y*w + x)*ch;
            px[o+0] = (uint8_t)(128 + 90*sin(x/25.0)*cos(y/25.0));
            px[o+1] = (uint8_t)(128 + 80*sin(y/18.0));
            px[o+2] = (uint8_t)(128 + 70*cos((x+y)/30.0));
        }
    uint8_t *blob, *out; size_t bl, ol;
    CHECK(wv_compress_image(px, w, h, ch, 0, WV_WAVELET_AUTO, -1, -1, &blob, &bl) == WV_OK, "compress ok");
    wv_info info; wv_inspect(blob, bl, &info);
    CHECK(info.ndim == 2 && info.width == w && info.height == h && info.channels == ch, "geometry preserved");
    CHECK(wv_decompress(blob, bl, &out, &ol) == WV_OK, "decompress ok");
    CHECK(ol == n && memcmp(px, out, n) == 0, "bit-exact roundtrip");
    printf("  %ux%u x%d (%zu B) -> %zu B\n", w, h, ch, n, bl);
    wv_free(blob); wv_free(out); free(px);
}

static void test_2d_lossy(void) {
    printf("[2D lossy image]\n");
    uint32_t w = 256, h = 256; int ch = 1;
    size_t n = (size_t)w*h*ch;
    uint8_t *px = malloc(n);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            px[(size_t)y*w + x] = (uint8_t)(128 + 90*sin(x/25.0)*cos(y/25.0));
    uint8_t *blob, *out; size_t bl, ol;
    wv_compress_image(px, w, h, ch, 1, WV_WAVELET_AUTO, -1, 85, &blob, &bl);
    wv_decompress(blob, bl, &out, &ol);
    CHECK(ol == n, "length preserved");
    CHECK(bl < n / 4, "strong compression (< 25%)");
    double e = rmse(px, out, n);
    CHECK(e < 8.0, "rmse < 8");
    printf("  %ux%u (%zu B) -> %zu B, rmse=%.3f\n", w, h, n, bl, e);
    wv_free(blob); wv_free(out); free(px);
}

int main(void) {
    printf("wavelet library %s self-test\n\n", wv_version());
    test_1d_lossless();
    test_1d_lossy();
    test_1d_random();
    test_2d_lossless();
    test_2d_lossy();
    printf("\n%s\n", failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return failures ? 1 : 0;
}
