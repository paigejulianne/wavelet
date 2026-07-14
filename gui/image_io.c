/* image_io.c -- see image_io.h */
#include "image_io.h"
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------------- helpers ---------------- */

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *len = got;
    return buf;
}

static const char *ext_of(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}
static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

/* ---------------- BMP ---------------- */

static int load_bmp(const uint8_t *d, size_t n, Image *out) {
    if (n < 54 || d[0] != 'B' || d[1] != 'M') return 0;
    uint32_t off   = rd32(d + 10);
    uint32_t hsize = rd32(d + 14);
    int32_t  w     = (int32_t)rd32(d + 18);
    int32_t  h     = (int32_t)rd32(d + 22);
    uint16_t bpp   = rd16(d + 28);
    uint32_t comp  = rd32(d + 30);         /* biCompression (BI_RGB == 0) */
    if (hsize < 40 || comp != 0) return 0;              /* uncompressed only */
    if (w <= 0 || h == 0) return 0;
    int top_down = h < 0;
    int H = top_down ? -h : h;
    int W = w;
    if (bpp != 24 && bpp != 8) return 0;

    /* palette for 8-bit (BGRA quads) starts at 14 + hsize */
    const uint8_t *pal = d + 14 + hsize;

    int channels = 3;
    uint8_t *px = (uint8_t *)malloc((size_t)W * H * 3);
    if (!px) return 0;

    size_t row_bytes = (bpp == 24) ? (((size_t)W * 3 + 3) & ~(size_t)3)
                                   : (((size_t)W     + 3) & ~(size_t)3);
    for (int y = 0; y < H; y++) {
        int srcy = top_down ? y : (H - 1 - y);
        const uint8_t *row = d + off + (size_t)srcy * row_bytes;
        if (row + row_bytes > d + n) { free(px); return 0; }
        uint8_t *dst = px + (size_t)y * W * 3;
        if (bpp == 24) {
            for (int x = 0; x < W; x++) {
                dst[x*3+0] = row[x*3+2];   /* R */
                dst[x*3+1] = row[x*3+1];   /* G */
                dst[x*3+2] = row[x*3+0];   /* B */
            }
        } else {
            for (int x = 0; x < W; x++) {
                uint8_t idx = row[x];
                const uint8_t *e = pal + (size_t)idx * 4;
                dst[x*3+0] = e[2];         /* R */
                dst[x*3+1] = e[1];         /* G */
                dst[x*3+2] = e[0];         /* B */
            }
        }
    }
    out->w = W; out->h = H; out->channels = channels; out->px = px;
    return 1;
}

static int save_bmp(const char *path, const Image *im) {
    int W = im->w, H = im->h, ch = im->channels;
    size_t row_bytes = (((size_t)W * 3 + 3) & ~(size_t)3);
    size_t img_size = row_bytes * H;
    uint32_t off = 54;
    uint32_t filesize = off + (uint32_t)img_size;

    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint8_t hdr[54]; memset(hdr, 0, sizeof hdr);
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=(uint8_t)filesize; hdr[3]=(uint8_t)(filesize>>8); hdr[4]=(uint8_t)(filesize>>16); hdr[5]=(uint8_t)(filesize>>24);
    hdr[10]=(uint8_t)off;
    hdr[14]=40;
    hdr[18]=(uint8_t)W; hdr[19]=(uint8_t)(W>>8); hdr[20]=(uint8_t)(W>>16); hdr[21]=(uint8_t)(W>>24);
    /* negative height => top-down */
    { int32_t nh = -H; uint32_t u=(uint32_t)nh; hdr[22]=(uint8_t)u; hdr[23]=(uint8_t)(u>>8); hdr[24]=(uint8_t)(u>>16); hdr[25]=(uint8_t)(u>>24); }
    hdr[26]=1;            /* planes */
    hdr[28]=24;           /* bpp */
    { uint32_t is=(uint32_t)img_size; hdr[34]=(uint8_t)is; hdr[35]=(uint8_t)(is>>8); hdr[36]=(uint8_t)(is>>16); hdr[37]=(uint8_t)(is>>24); }
    fwrite(hdr, 1, 54, f);

    uint8_t *row = (uint8_t *)calloc(1, row_bytes);
    if (!row) { fclose(f); return 0; }
    for (int y = 0; y < H; y++) {
        const uint8_t *s = im->px + (size_t)y * W * ch;
        for (int x = 0; x < W; x++) {
            uint8_t r, g, b;
            if (ch == 1) { r = g = b = s[x]; }
            else { r = s[x*3+0]; g = s[x*3+1]; b = s[x*3+2]; }
            row[x*3+0] = b; row[x*3+1] = g; row[x*3+2] = r;
        }
        fwrite(row, 1, row_bytes, f);
    }
    free(row);
    fclose(f);
    return 1;
}

/* ---------------- netpbm ---------------- */

static int nb_uint(const uint8_t *p, size_t n, size_t *pos, int *val) {
    for (;;) {
        while (*pos < n && isspace(p[*pos])) (*pos)++;
        if (*pos < n && p[*pos] == '#') { while (*pos < n && p[*pos] != '\n') (*pos)++; }
        else break;
    }
    if (*pos >= n || !isdigit(p[*pos])) return 0;
    int v = 0;
    while (*pos < n && isdigit(p[*pos])) v = v*10 + (p[(*pos)++]-'0');
    *val = v;
    return 1;
}

static int load_netpbm(const uint8_t *d, size_t n, Image *out) {
    if (n < 2 || d[0] != 'P' || (d[1] != '5' && d[1] != '6')) return 0;
    int ch = (d[1] == '5') ? 1 : 3;
    size_t pos = 2;
    int W, H, maxv;
    if (!nb_uint(d, n, &pos, &W) || !nb_uint(d, n, &pos, &H) || !nb_uint(d, n, &pos, &maxv)) return 0;
    if (maxv != 255 || W <= 0 || H <= 0) return 0;
    pos++;                                   /* single whitespace */
    size_t need = (size_t)W * H * ch;
    if (pos + need > n) return 0;
    uint8_t *px = (uint8_t *)malloc(need);
    if (!px) return 0;
    memcpy(px, d + pos, need);
    out->w = W; out->h = H; out->channels = ch; out->px = px;
    return 1;
}

static int save_netpbm(const char *path, const Image *im, int want_ch) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P%c\n%d %d\n255\n", want_ch == 1 ? '5' : '6', im->w, im->h);
    for (int i = 0; i < im->w * im->h; i++) {
        uint8_t r, g, b;
        if (im->channels == 1) { r = g = b = im->px[i]; }
        else { r = im->px[i*3+0]; g = im->px[i*3+1]; b = im->px[i*3+2]; }
        if (want_ch == 1) { uint8_t gray = (uint8_t)((r*30 + g*59 + b*11)/100); fputc(gray, f); }
        else { fputc(r, f); fputc(g, f); fputc(b, f); }
    }
    fclose(f);
    return 1;
}

/* ---------------- public ---------------- */

int img_load(const char *path, Image *out) {
    memset(out, 0, sizeof *out);
    size_t n; uint8_t *d = read_all(path, &n);
    if (!d) return 0;
    int ok = 0;
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
        ok = png_decode(d, n, out);
    else if (n >= 2 && d[0] == 'B' && d[1] == 'M') ok = load_bmp(d, n, out);
    else if (n >= 2 && d[0] == 'P')                ok = load_netpbm(d, n, out);
    free(d);
    return ok;
}

int img_save(const char *path, const Image *im) {
    if (!im->px) return 0;
    const char *e = ext_of(path);
    if (ieq(e, "bmp")) return save_bmp(path, im);
    if (ieq(e, "pgm")) return save_netpbm(path, im, 1);
    if (ieq(e, "ppm")) return save_netpbm(path, im, 3);
    return save_bmp(path, im);               /* default */
}

void img_free(Image *im) { free(im->px); im->px = NULL; im->w = im->h = im->channels = 0; }

int img_copy(Image *dst, const Image *src) {
    size_t n = (size_t)src->w * src->h * src->channels;
    dst->px = (uint8_t *)malloc(n ? n : 1);
    if (!dst->px) return 0;
    memcpy(dst->px, src->px, n);
    dst->w = src->w; dst->h = src->h; dst->channels = src->channels;
    return 1;
}

uint8_t *img_to_bgr24(const Image *im) {
    if (!im->px) return NULL;
    size_t n = (size_t)im->w * im->h;
    uint8_t *out = (uint8_t *)malloc(n * 3);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        uint8_t r, g, b;
        if (im->channels == 1) { r = g = b = im->px[i]; }
        else { r = im->px[i*3+0]; g = im->px[i*3+1]; b = im->px[i*3+2]; }
        out[i*3+0] = b; out[i*3+1] = g; out[i*3+2] = r;
    }
    return out;
}
