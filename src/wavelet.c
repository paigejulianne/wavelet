/*
 * wavelet.c -- implementation of the wavelet compressor library.
 *
 * Self-contained: depends only on the C standard library. The entropy stage
 * is an order-0 adaptive range coder (Subbotin-style), so there is no zlib or
 * other third-party dependency to link on any platform.
 */
#ifndef WV_BUILD_SHARED
#define WV_BUILD_SHARED
#endif
#include "wavelet.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WV_VERSION_STR "1.0.0"
#define WV_FORMAT_VERSION 1

/* Container mode byte values (mirror enum wv_mode). */
#define MODE_STORED_RAW 0
#define MODE_LOSSLESS   1
#define MODE_LOSSY      2
#define MODE_STORED_RC  3

/* CDF 9/7 lifting constants. */
static const double A1 = -1.586134342059924;
static const double A2 = -0.052980118572961;
static const double A3 =  0.882911075530934;
static const double A4 =  0.443506852043971;
static const double K  =  1.230174104914001;

/* ---------------------------------------------------------------------- */
/* Growable byte buffer                                                    */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t   len;
    size_t   cap;
    int      oom;
} buf;

static void buf_init(buf *b) { b->p = NULL; b->len = b->cap = 0; b->oom = 0; }

static int buf_reserve(buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 1;
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + extra) ncap *= 2;
    uint8_t *np = (uint8_t *)realloc(b->p, ncap);
    if (!np) { b->oom = 1; return 0; }
    b->p = np; b->cap = ncap;
    return 1;
}

static void buf_push(buf *b, uint8_t v) {
    if (buf_reserve(b, 1)) b->p[b->len++] = v;
}

static void buf_append(buf *b, const void *data, size_t n) {
    if (buf_reserve(b, n)) { memcpy(b->p + b->len, data, n); b->len += n; }
}

/* ---------------------------------------------------------------------- */
/* Little-endian fixed-width writers / readers                             */
/* ---------------------------------------------------------------------- */

static void put_u32(buf *b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) };
    buf_append(b, t, 4);
}
static void put_u64(buf *b, uint64_t v) {
    uint8_t t[8];
    for (int i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8*i));
    buf_append(b, t, 8);
}
static void put_f64(buf *b, double d) {
    uint64_t u; memcpy(&u, &d, 8); put_u64(b, u);
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8*i); return v;
}
static double get_f64(const uint8_t *p) {
    uint64_t u = get_u64(p); double d; memcpy(&d, &u, 8); return d;
}

/* ---------------------------------------------------------------------- */
/* CRC-32 (IEEE 802.3)                                                     */
/* ---------------------------------------------------------------------- */

static uint32_t crc32_table[256];
static int crc32_ready = 0;

static void crc32_build(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

static uint32_t crc32_calc(const uint8_t *data, size_t len) {
    if (!crc32_ready) crc32_build();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------------- */
/* Order-0 adaptive range coder (Subbotin carryless)                       */
/* ---------------------------------------------------------------------- */

#define RC_TOP (1u << 24)
#define RC_BOT (1u << 16)
#define MODEL_STEP    24
#define MODEL_MAXTOT  (1u << 15)

/*
 * Frequencies are kept both as a flat table (for O(1) point access) and in a
 * Fenwick / binary-indexed tree over the 256 symbols, so cumulative-sum and
 * symbol-by-cumulative lookups are O(log 256) = 8 steps instead of O(256).
 * tree[] is 1-indexed: symbol s lives at index s+1.
 */
typedef struct {
    uint16_t freq[256];
    uint32_t tree[257];
    uint32_t tot;
} model;

static void fen_add(model *m, int sym, int delta) {
    for (int x = sym + 1; x <= 256; x += x & (-x)) m->tree[x] += (uint32_t)delta;
}
/* sum of freq[0 .. count-1] */
static uint32_t fen_prefix(const model *m, int count) {
    uint32_t s = 0;
    for (int x = count; x > 0; x -= x & (-x)) s += m->tree[x];
    return s;
}
static void fen_rebuild(model *m) {
    memset(m->tree, 0, sizeof m->tree);
    for (int i = 0; i < 256; i++) fen_add(m, i, m->freq[i]);
}

static void model_init(model *m) {
    for (int i = 0; i < 256; i++) m->freq[i] = 1;
    fen_rebuild(m);
    m->tot = 256;
}
static void model_update(model *m, int sym) {
    m->freq[sym] = (uint16_t)(m->freq[sym] + MODEL_STEP);
    fen_add(m, sym, MODEL_STEP);
    m->tot += MODEL_STEP;
    if (m->tot >= MODEL_MAXTOT) {         /* periodic rescale keeps tot < RC_BOT */
        m->tot = 0;
        for (int i = 0; i < 256; i++) {
            m->freq[i] = (uint16_t)((m->freq[i] + 1) >> 1);
            m->tot += m->freq[i];
        }
        fen_rebuild(m);
    }
}
static uint32_t model_cum(const model *m, int sym) {
    return fen_prefix(m, sym);
}
/* find the symbol whose cumulative interval contains value v (< tot) */
static int model_find(const model *m, uint32_t v, uint32_t *cum, uint32_t *freq) {
    int pos = 0; uint32_t acc = 0;
    for (int bit = 256; bit; bit >>= 1) {          /* 256 = 2^8 */
        int nx = pos + bit;
        if (nx <= 256 && acc + m->tree[nx] <= v) { pos = nx; acc += m->tree[nx]; }
    }
    *cum = acc; *freq = m->freq[pos];              /* pos in [0,255] */
    return pos;
}

/* --- encoder --- */
typedef struct { uint32_t low, range; buf *out; } rc_enc;

static void rc_enc_init(rc_enc *e, buf *out) { e->low = 0; e->range = 0xFFFFFFFFu; e->out = out; }

static void rc_enc_renorm(rc_enc *e) {
    while ((e->low ^ (e->low + e->range)) < RC_TOP ||
           (e->range < RC_BOT && ((e->range = (0u - e->low) & (RC_BOT - 1)), 1))) {
        buf_push(e->out, (uint8_t)(e->low >> 24));
        e->low <<= 8;
        e->range <<= 8;
    }
}
static void rc_enc_encode(rc_enc *e, uint32_t cum, uint32_t freq, uint32_t tot) {
    e->range /= tot;
    e->low   += cum * e->range;
    e->range *= freq;
    rc_enc_renorm(e);
}
static void rc_enc_flush(rc_enc *e) {
    for (int i = 0; i < 4; i++) { buf_push(e->out, (uint8_t)(e->low >> 24)); e->low <<= 8; }
}

/* --- decoder --- */
typedef struct { uint32_t low, range, code; const uint8_t *in; size_t pos, len; } rc_dec;

static uint8_t rc_getbyte(rc_dec *d) { return d->pos < d->len ? d->in[d->pos++] : 0; }

static void rc_dec_init(rc_dec *d, const uint8_t *in, size_t len) {
    d->low = 0; d->range = 0xFFFFFFFFu; d->code = 0; d->in = in; d->pos = 0; d->len = len;
    for (int i = 0; i < 4; i++) d->code = (d->code << 8) | rc_getbyte(d);
}
static uint32_t rc_dec_getfreq(rc_dec *d, uint32_t tot) {
    d->range /= tot;
    return (d->code - d->low) / d->range;
}
static void rc_dec_decode(rc_dec *d, uint32_t cum, uint32_t freq) {
    d->low   += cum * d->range;
    d->range *= freq;
    while ((d->low ^ (d->low + d->range)) < RC_TOP ||
           (d->range < RC_BOT && ((d->range = (0u - d->low) & (RC_BOT - 1)), 1))) {
        d->code = (d->code << 8) | rc_getbyte(d);
        d->low <<= 8;
        d->range <<= 8;
    }
}

/* Range-code a byte stream (order-0 adaptive). */
static void rc_compress(const uint8_t *data, size_t n, buf *out) {
    model m; model_init(&m);
    rc_enc e; rc_enc_init(&e, out);
    for (size_t i = 0; i < n; i++) {
        int s = data[i];
        rc_enc_encode(&e, model_cum(&m, s), m.freq[s], m.tot);
        model_update(&m, s);
    }
    rc_enc_flush(&e);
}
/* Inverse: produce exactly n bytes into out (must have capacity). */
static void rc_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t n) {
    model m; model_init(&m);
    rc_dec d; rc_dec_init(&d, in, in_len);
    for (size_t i = 0; i < n; i++) {
        uint32_t v = rc_dec_getfreq(&d, m.tot);
        uint32_t cum, freq;
        int s = model_find(&m, v, &cum, &freq);
        rc_dec_decode(&d, cum, freq);
        out[i] = (uint8_t)s;
        model_update(&m, s);
    }
}

/* ---------------------------------------------------------------------- */
/* zigzag + LEB128 varint over int64                                       */
/* ---------------------------------------------------------------------- */

static void varint_put(buf *b, int64_t v) {
    uint64_t z = (v >= 0) ? ((uint64_t)v << 1) : (((uint64_t)(-(v + 1)) << 1) | 1);
    for (;;) {
        uint8_t byte = (uint8_t)(z & 0x7F);
        z >>= 7;
        if (z) buf_push(b, byte | 0x80);
        else { buf_push(b, byte); break; }
    }
}
/* Parse `count` varints from buf; returns bytes consumed or -1 on overrun. */
static int64_t varint_get_all(const uint8_t *p, size_t n, int64_t *out, size_t count) {
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        uint64_t z = 0; int shift = 0;
        for (;;) {
            if (pos >= n) return -1;
            uint8_t byte = p[pos++];
            z |= (uint64_t)(byte & 0x7F) << shift;
            if (!(byte & 0x80)) break;
            shift += 7;
            if (shift > 63) return -1;
        }
        out[i] = (z & 1) ? -(int64_t)(z >> 1) - 1 : (int64_t)(z >> 1);
    }
    return (int64_t)pos;
}

/* ---------------------------------------------------------------------- */
/* Lifting primitives (periodic boundary)                                  */
/*                                                                         */
/* Each *_1d transforms a contiguous vector of even length n in place and  */
/* leaves it packed as [ low(n/2) | high(n/2) ]. `tmp` needs n slots.      */
/* ---------------------------------------------------------------------- */

/* floor(a/2) and floor((a)/4) matching Python's arithmetic shift on ints. */
static inline int64_t fdiv2(int64_t a) { return (a >= 0) ? (a >> 1) : -(((-a) + 1) >> 1); }
static inline int64_t fdiv4(int64_t a) { return (a >= 0) ? (a >> 2) : -(((-a) + 3) >> 2); }

static void dwt53_fwd_1d(int64_t *x, int n, int64_t *tmp) {
    for (int i = 1; i < n; i += 2)                 /* predict odd */
        x[i] -= fdiv2(x[i-1] + x[(i+1) % n]);
    for (int i = 0; i < n; i += 2) {               /* update even */
        int64_t l = (i == 0) ? x[n-1] : x[i-1];
        x[i] += fdiv4(l + x[i+1] + 2);
    }
    int h = n / 2;
    for (int k = 0; k < h; k++) { tmp[k] = x[2*k]; tmp[h+k] = x[2*k+1]; }
    memcpy(x, tmp, (size_t)n * sizeof(int64_t));
}
static void dwt53_inv_1d(int64_t *x, int n, int64_t *tmp) {
    int h = n / 2;
    for (int k = 0; k < h; k++) { tmp[2*k] = x[k]; tmp[2*k+1] = x[h+k]; }
    memcpy(x, tmp, (size_t)n * sizeof(int64_t));
    for (int i = 0; i < n; i += 2) {               /* undo update even */
        int64_t l = (i == 0) ? x[n-1] : x[i-1];
        x[i] -= fdiv4(l + x[i+1] + 2);
    }
    for (int i = 1; i < n; i += 2)                 /* undo predict odd */
        x[i] += fdiv2(x[i-1] + x[(i+1) % n]);
}

static void dwt97_fwd_1d(double *x, int n, double *tmp) {
    for (int i = 1; i < n; i += 2) x[i] += A1 * (x[i-1] + x[(i+1) % n]);
    for (int i = 0; i < n; i += 2) x[i] += A2 * ((i == 0 ? x[n-1] : x[i-1]) + x[i+1]);
    for (int i = 1; i < n; i += 2) x[i] += A3 * (x[i-1] + x[(i+1) % n]);
    for (int i = 0; i < n; i += 2) x[i] += A4 * ((i == 0 ? x[n-1] : x[i-1]) + x[i+1]);
    int h = n / 2;
    for (int k = 0; k < h; k++) { tmp[k] = x[2*k] / K; tmp[h+k] = x[2*k+1] * K; }
    memcpy(x, tmp, (size_t)n * sizeof(double));
}
static void dwt97_inv_1d(double *x, int n, double *tmp) {
    int h = n / 2;
    for (int k = 0; k < h; k++) { tmp[2*k] = x[k] * K; tmp[2*k+1] = x[h+k] / K; }
    memcpy(x, tmp, (size_t)n * sizeof(double));
    for (int i = 0; i < n; i += 2) x[i] -= A4 * ((i == 0 ? x[n-1] : x[i-1]) + x[i+1]);
    for (int i = 1; i < n; i += 2) x[i] -= A3 * (x[i-1] + x[(i+1) % n]);
    for (int i = 0; i < n; i += 2) x[i] -= A2 * ((i == 0 ? x[n-1] : x[i-1]) + x[i+1]);
    for (int i = 1; i < n; i += 2) x[i] -= A1 * (x[i-1] + x[(i+1) % n]);
}

/* ---------------------------------------------------------------------- */
/* Multi-level transforms                                                  */
/* ---------------------------------------------------------------------- */

static void mlevel_1d_53_fwd(int64_t *x, int n, int levels, int64_t *tmp) {
    int cur = n;
    for (int l = 0; l < levels; l++) { dwt53_fwd_1d(x, cur, tmp); cur >>= 1; }
}
static void mlevel_1d_53_inv(int64_t *x, int n, int levels, int64_t *tmp) {
    for (int l = levels - 1; l >= 0; l--) { int cur = n >> l; dwt53_inv_1d(x, cur, tmp); }
}
static void mlevel_1d_97_fwd(double *x, int n, int levels, double *tmp) {
    int cur = n;
    for (int l = 0; l < levels; l++) { dwt97_fwd_1d(x, cur, tmp); cur >>= 1; }
}
static void mlevel_1d_97_inv(double *x, int n, int levels, double *tmp) {
    for (int l = levels - 1; l >= 0; l--) { int cur = n >> l; dwt97_inv_1d(x, cur, tmp); }
}

/* Separable 2D on a Wp x Hp plane (row-major, stride Wp). */
static void mlevel_2d_53_fwd(int64_t *P, int Wp, int Hp, int levels,
                             int64_t *rowtmp, int64_t *col, int64_t *coltmp) {
    for (int l = 0; l < levels; l++) {
        int aw = Wp >> l, ah = Hp >> l;
        for (int r = 0; r < ah; r++) dwt53_fwd_1d(&P[(size_t)r * Wp], aw, rowtmp);
        for (int c = 0; c < aw; c++) {
            for (int k = 0; k < ah; k++) col[k] = P[(size_t)k * Wp + c];
            dwt53_fwd_1d(col, ah, coltmp);
            for (int k = 0; k < ah; k++) P[(size_t)k * Wp + c] = col[k];
        }
    }
}
static void mlevel_2d_53_inv(int64_t *P, int Wp, int Hp, int levels,
                             int64_t *rowtmp, int64_t *col, int64_t *coltmp) {
    for (int l = levels - 1; l >= 0; l--) {
        int aw = Wp >> l, ah = Hp >> l;
        for (int c = 0; c < aw; c++) {
            for (int k = 0; k < ah; k++) col[k] = P[(size_t)k * Wp + c];
            dwt53_inv_1d(col, ah, coltmp);
            for (int k = 0; k < ah; k++) P[(size_t)k * Wp + c] = col[k];
        }
        for (int r = 0; r < ah; r++) dwt53_inv_1d(&P[(size_t)r * Wp], aw, rowtmp);
    }
}
static void mlevel_2d_97_fwd(double *P, int Wp, int Hp, int levels,
                             double *rowtmp, double *col, double *coltmp) {
    for (int l = 0; l < levels; l++) {
        int aw = Wp >> l, ah = Hp >> l;
        for (int r = 0; r < ah; r++) dwt97_fwd_1d(&P[(size_t)r * Wp], aw, rowtmp);
        for (int c = 0; c < aw; c++) {
            for (int k = 0; k < ah; k++) col[k] = P[(size_t)k * Wp + c];
            dwt97_fwd_1d(col, ah, coltmp);
            for (int k = 0; k < ah; k++) P[(size_t)k * Wp + c] = col[k];
        }
    }
}
static void mlevel_2d_97_inv(double *P, int Wp, int Hp, int levels,
                             double *rowtmp, double *col, double *coltmp) {
    for (int l = levels - 1; l >= 0; l--) {
        int aw = Wp >> l, ah = Hp >> l;
        for (int c = 0; c < aw; c++) {
            for (int k = 0; k < ah; k++) col[k] = P[(size_t)k * Wp + c];
            dwt97_inv_1d(col, ah, coltmp);
            for (int k = 0; k < ah; k++) P[(size_t)k * Wp + c] = col[k];
        }
        for (int r = 0; r < ah; r++) dwt97_inv_1d(&P[(size_t)r * Wp], aw, rowtmp);
    }
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ---------------------------------------------------------------------- */

static int auto_levels_1d(size_t n) {
    int levels = 0;
    while (n >= 2 && levels < 12) { n >>= 1; levels++; }
    return levels < 1 ? 1 : levels;
}
static int auto_levels_2d(uint32_t w, uint32_t h) {
    uint32_t m = w < h ? w : h;
    int levels = 0;
    while (m >= 2 && levels < 12) { m >>= 1; levels++; }
    return levels < 1 ? 1 : levels;
}
static size_t round_up(size_t n, size_t mult) {
    size_t r = n % mult;
    return r ? n + (mult - r) : n;
}
static double quality_to_step(int quality) {
    if (quality <= 0) quality = 75;
    if (quality > 100) quality = 100;
    double s = (101 - quality) / 2.0;
    return s < 0.25 ? 0.25 : s;
}
static int clamp_byte(double v) {
    long r = (long)lround(v);
    if (r < 0) return 0;
    if (r > 255) return 255;
    return (int)r;
}

/* Reflect-pad `src` of length n into dst of length total (dst[0..n) = src). */
static void reflect_pad_i64(int64_t *dst, const uint8_t *src, size_t n, size_t total) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
    for (size_t i = n; i < total; i++) {
        size_t j = (i - n) % n;
        dst[i] = src[n - 1 - j];
    }
}

/* ---------------------------------------------------------------------- */
/* Container header                                                        */
/* ---------------------------------------------------------------------- */

#define HDR_SIZE 56
static const uint8_t MAGIC[4] = { 'W', 'V', 'L', 'C' };

typedef struct {
    int      mode, wavelet, levels, ndim, channels;
    uint32_t width, height, crc;
    uint64_t original_len, coeff_count, sym_len; /* sym_len = decoded symbol count */
    double   quant_step;
} hdr;

static void write_header(buf *b, const hdr *h) {
    buf_append(b, MAGIC, 4);
    buf_push(b, WV_FORMAT_VERSION);
    buf_push(b, (uint8_t)h->mode);
    buf_push(b, (uint8_t)h->wavelet);
    buf_push(b, (uint8_t)h->levels);
    buf_push(b, (uint8_t)h->ndim);
    buf_push(b, (uint8_t)h->channels);
    buf_push(b, 0); buf_push(b, 0);              /* reserved */
    put_u32(b, h->crc);
    put_u32(b, h->width);
    put_u32(b, h->height);
    put_u64(b, h->original_len);
    put_f64(b, h->quant_step);
    put_u64(b, h->coeff_count);
    put_u64(b, h->sym_len);
}

static int read_header(const uint8_t *p, size_t len, hdr *h, int *version) {
    if (len < HDR_SIZE) return WV_ERR_CORRUPT;
    if (memcmp(p, MAGIC, 4) != 0) return WV_ERR_MAGIC;
    *version = p[4];
    if (*version != WV_FORMAT_VERSION) return WV_ERR_VERSION;
    h->mode        = p[5];
    h->wavelet     = p[6];
    h->levels      = p[7];
    h->ndim        = p[8];
    h->channels    = p[9];
    h->crc         = get_u32(p + 12);
    h->width       = get_u32(p + 16);
    h->height      = get_u32(p + 20);
    h->original_len= get_u64(p + 24);
    h->quant_step  = get_f64(p + 32);
    h->coeff_count = get_u64(p + 40);
    h->sym_len     = get_u64(p + 48);
    return WV_OK;
}

/* ---------------------------------------------------------------------- */
/* Emit a container from a finished coefficient list                       */
/* ---------------------------------------------------------------------- */

/* Build the varint symbol stream, range-code it, wrap in a container.
 * Returns a malloc'd blob via *out or NULL on OOM. */
static int emit_coeffs(const hdr *base, const int64_t *coeffs, size_t count,
                       uint8_t **out, size_t *out_len) {
    buf sym; buf_init(&sym);
    for (size_t i = 0; i < count; i++) varint_put(&sym, coeffs[i]);
    if (sym.oom) { free(sym.p); return WV_ERR_OOM; }

    buf body; buf_init(&body);
    rc_compress(sym.p, sym.len, &body);
    if (body.oom) { free(sym.p); free(body.p); return WV_ERR_OOM; }

    hdr h = *base;
    h.coeff_count = count;
    h.sym_len = sym.len;

    buf out_b; buf_init(&out_b);
    write_header(&out_b, &h);
    buf_append(&out_b, body.p, body.len);
    free(sym.p); free(body.p);
    if (out_b.oom) { free(out_b.p); return WV_ERR_OOM; }

    *out = out_b.p; *out_len = out_b.len;
    return WV_OK;
}

/* Emit a STORED_RAW container (payload verbatim). */
static int emit_stored_raw(const uint8_t *data, size_t len, uint32_t crc,
                           int ndim, int channels, uint32_t w, uint32_t h,
                           uint8_t **out, size_t *out_len) {
    hdr h0; memset(&h0, 0, sizeof h0);
    h0.mode = MODE_STORED_RAW; h0.wavelet = 0; h0.levels = 0;
    h0.ndim = ndim; h0.channels = channels; h0.crc = crc;
    h0.width = w; h0.height = h; h0.original_len = len;
    h0.quant_step = 1.0; h0.coeff_count = 0; h0.sym_len = len;

    buf out_b; buf_init(&out_b);
    write_header(&out_b, &h0);
    buf_append(&out_b, data, len);
    if (out_b.oom) { free(out_b.p); return WV_ERR_OOM; }
    *out = out_b.p; *out_len = out_b.len;
    return WV_OK;
}

/* ---------------------------------------------------------------------- */
/* Public: 1D compress                                                     */
/* ---------------------------------------------------------------------- */

WV_API int wv_compress(const uint8_t *data, size_t len,
                       int lossy, int wavelet, int levels, int quality,
                       uint8_t **out, size_t *out_len) {
    if (!data && len) return WV_ERR_ARG;
    if (!out || !out_len) return WV_ERR_ARG;

    uint32_t crc = crc32_calc(data, len);

    if (len == 0)
        return emit_stored_raw(data, 0, crc, 1, 1, 0, 0, out, out_len);

    if (wavelet == WV_WAVELET_AUTO) wavelet = lossy ? WV_WAVELET_97 : WV_WAVELET_53;
    if (lossy) wavelet = WV_WAVELET_97; /* lossless requires the reversible 5/3 */

    int max_lv = auto_levels_1d(len);
    if (levels <= 0 || levels > max_lv) levels = max_lv;

    size_t block = (size_t)1 << levels;
    size_t Lp = round_up(len, block);

    double step = quality_to_step(quality);

    int rc = WV_ERR_OOM;
    int64_t *coeffs = (int64_t *)malloc(Lp * sizeof(int64_t));
    int64_t *tmp    = (int64_t *)malloc(Lp * sizeof(int64_t));
    if (!coeffs || !tmp) { free(coeffs); free(tmp); return WV_ERR_OOM; }

    hdr base; memset(&base, 0, sizeof base);
    base.wavelet = wavelet; base.levels = levels; base.ndim = 1; base.channels = 1;
    base.crc = crc; base.width = 0; base.height = 0; base.original_len = len;

    if (!lossy) {
        reflect_pad_i64(coeffs, data, len, Lp);
        mlevel_1d_53_fwd(coeffs, (int)Lp, levels, tmp);
        base.mode = MODE_LOSSLESS; base.quant_step = 1.0;
        rc = emit_coeffs(&base, coeffs, Lp, out, out_len);
    } else {
        double *dc = (double *)malloc(Lp * sizeof(double));
        double *dt = (double *)malloc(Lp * sizeof(double));
        if (!dc || !dt) { free(dc); free(dt); free(coeffs); free(tmp); return WV_ERR_OOM; }
        for (size_t i = 0; i < len; i++) dc[i] = (double)data[i];
        for (size_t i = len; i < Lp; i++) dc[i] = (double)data[len - 1 - ((i - len) % len)];
        mlevel_1d_97_fwd(dc, (int)Lp, levels, dt);
        for (size_t i = 0; i < Lp; i++) coeffs[i] = (int64_t)llround(dc[i] / step);
        base.mode = MODE_LOSSY; base.quant_step = step;
        rc = emit_coeffs(&base, coeffs, Lp, out, out_len);
        free(dc); free(dt);
    }
    free(coeffs); free(tmp);
    if (rc != WV_OK) return rc;

    /* Never expand: for lossless, fall back to STORED_RAW if it is smaller. */
    if (!lossy && *out_len >= len + HDR_SIZE) {
        uint8_t *alt; size_t alt_len;
        if (emit_stored_raw(data, len, crc, 1, 1, 0, 0, &alt, &alt_len) == WV_OK) {
            if (alt_len < *out_len) { free(*out); *out = alt; *out_len = alt_len; }
            else free(alt);
        }
    }
    return WV_OK;
}

/* ---------------------------------------------------------------------- */
/* Public: 2D image compress                                               */
/* ---------------------------------------------------------------------- */

WV_API int wv_compress_image(const uint8_t *pixels,
                             uint32_t width, uint32_t height, int channels,
                             int lossy, int wavelet, int levels, int quality,
                             uint8_t **out, size_t *out_len) {
    if (!pixels || !out || !out_len) return WV_ERR_ARG;
    if (width == 0 || height == 0) return WV_ERR_ARG;
    if (channels != 1 && channels != 3) return WV_ERR_ARG;

    size_t total = (size_t)width * height * channels;
    uint32_t crc = crc32_calc(pixels, total);

    if (wavelet == WV_WAVELET_AUTO) wavelet = lossy ? WV_WAVELET_97 : WV_WAVELET_53;
    if (lossy) wavelet = WV_WAVELET_97;

    int max_lv = auto_levels_2d(width, height);
    if (levels <= 0 || levels > max_lv) levels = max_lv;

    size_t block = (size_t)1 << levels;
    int Wp = (int)round_up(width, block);
    int Hp = (int)round_up(height, block);
    size_t plane = (size_t)Wp * Hp;
    double step = quality_to_step(quality);

    size_t maxdim = (size_t)(Wp > Hp ? Wp : Hp);
    int64_t *coeffs = (int64_t *)malloc(plane * channels * sizeof(int64_t));
    if (!coeffs) return WV_ERR_OOM;

    hdr base; memset(&base, 0, sizeof base);
    base.wavelet = wavelet; base.levels = levels; base.ndim = 2; base.channels = channels;
    base.crc = crc; base.width = width; base.height = height; base.original_len = total;
    base.quant_step = lossy ? step : 1.0;
    base.mode = lossy ? MODE_LOSSY : MODE_LOSSLESS;

    int rc;
    if (!lossy) {
        int64_t *P   = (int64_t *)malloc(plane * sizeof(int64_t));
        int64_t *rt  = (int64_t *)malloc(maxdim * sizeof(int64_t));
        int64_t *col = (int64_t *)malloc(maxdim * sizeof(int64_t));
        int64_t *ct  = (int64_t *)malloc(maxdim * sizeof(int64_t));
        if (!P || !rt || !col || !ct) { free(P);free(rt);free(col);free(ct);free(coeffs); return WV_ERR_OOM; }
        for (int ch = 0; ch < channels; ch++) {
            for (uint32_t y = 0; y < (uint32_t)Hp; y++)
                for (uint32_t x = 0; x < (uint32_t)Wp; x++) {
                    uint32_t sx = x < width  ? x : width  - 1 - ((x - width)  % width);
                    uint32_t sy = y < height ? y : height - 1 - ((y - height) % height);
                    P[(size_t)y * Wp + x] =
                        pixels[((size_t)sy * width + sx) * channels + ch];
                }
            mlevel_2d_53_fwd(P, Wp, Hp, levels, rt, col, ct);
            memcpy(coeffs + (size_t)ch * plane, P, plane * sizeof(int64_t));
        }
        free(P); free(rt); free(col); free(ct);
        rc = emit_coeffs(&base, coeffs, plane * channels, out, out_len);
    } else {
        double *P   = (double *)malloc(plane * sizeof(double));
        double *rt  = (double *)malloc(maxdim * sizeof(double));
        double *col = (double *)malloc(maxdim * sizeof(double));
        double *ct  = (double *)malloc(maxdim * sizeof(double));
        if (!P || !rt || !col || !ct) { free(P);free(rt);free(col);free(ct);free(coeffs); return WV_ERR_OOM; }
        for (int ch = 0; ch < channels; ch++) {
            for (uint32_t y = 0; y < (uint32_t)Hp; y++)
                for (uint32_t x = 0; x < (uint32_t)Wp; x++) {
                    uint32_t sx = x < width  ? x : width  - 1 - ((x - width)  % width);
                    uint32_t sy = y < height ? y : height - 1 - ((y - height) % height);
                    P[(size_t)y * Wp + x] =
                        (double)pixels[((size_t)sy * width + sx) * channels + ch];
                }
            mlevel_2d_97_fwd(P, Wp, Hp, levels, rt, col, ct);
            for (size_t i = 0; i < plane; i++)
                coeffs[(size_t)ch * plane + i] = (int64_t)llround(P[i] / step);
        }
        free(P); free(rt); free(col); free(ct);
        rc = emit_coeffs(&base, coeffs, plane * channels, out, out_len);
    }
    free(coeffs);
    if (rc != WV_OK) return rc;

    if (!lossy && *out_len >= total + HDR_SIZE) {
        uint8_t *alt; size_t alt_len;
        if (emit_stored_raw(pixels, total, crc, 2, channels, width, height,
                            &alt, &alt_len) == WV_OK) {
            if (alt_len < *out_len) { free(*out); *out = alt; *out_len = alt_len; }
            else free(alt);
        }
    }
    return WV_OK;
}

/* ---------------------------------------------------------------------- */
/* Public: decompress                                                      */
/* ---------------------------------------------------------------------- */

WV_API int wv_decompress(const uint8_t *blob, size_t len,
                         uint8_t **out, size_t *out_len) {
    if (!blob || !out || !out_len) return WV_ERR_ARG;
    hdr h; int ver;
    int st = read_header(blob, len, &h, &ver);
    if (st != WV_OK) return st;

    const uint8_t *body = blob + HDR_SIZE;
    size_t body_len = len - HDR_SIZE;

    if (h.mode == MODE_STORED_RAW) {
        if (body_len < h.original_len) return WV_ERR_CORRUPT;
        uint8_t *o = (uint8_t *)malloc(h.original_len ? h.original_len : 1);
        if (!o) return WV_ERR_OOM;
        memcpy(o, body, h.original_len);
        if (crc32_calc(o, h.original_len) != h.crc) { free(o); return WV_ERR_CRC; }
        *out = o; *out_len = h.original_len;
        return WV_OK;
    }

    /* Range-decode the symbol stream. */
    uint8_t *sym = (uint8_t *)malloc(h.sym_len ? (size_t)h.sym_len : 1);
    if (!sym) return WV_ERR_OOM;
    rc_decompress(body, body_len, sym, (size_t)h.sym_len);

    /* Parse coefficients. */
    size_t count = (size_t)h.coeff_count;
    int64_t *coeffs = (int64_t *)malloc((count ? count : 1) * sizeof(int64_t));
    if (!coeffs) { free(sym); return WV_ERR_OOM; }
    if (varint_get_all(sym, (size_t)h.sym_len, coeffs, count) < 0) {
        free(sym); free(coeffs); return WV_ERR_CORRUPT;
    }
    free(sym);

    int st2 = WV_OK;
    if (h.ndim == 1) {
        size_t Lp = count;
        uint8_t *o = (uint8_t *)malloc(h.original_len ? (size_t)h.original_len : 1);
        if (!o) { free(coeffs); return WV_ERR_OOM; }
        if (h.mode == MODE_LOSSLESS) {
            int64_t *tmp = (int64_t *)malloc(Lp * sizeof(int64_t));
            if (!tmp) { free(o); free(coeffs); return WV_ERR_OOM; }
            mlevel_1d_53_inv(coeffs, (int)Lp, h.levels, tmp);
            for (size_t i = 0; i < h.original_len; i++) {
                int64_t v = coeffs[i];
                o[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
            free(tmp);
        } else {
            double *dc = (double *)malloc(Lp * sizeof(double));
            double *dt = (double *)malloc(Lp * sizeof(double));
            if (!dc || !dt) { free(dc); free(dt); free(o); free(coeffs); return WV_ERR_OOM; }
            for (size_t i = 0; i < Lp; i++) dc[i] = (double)coeffs[i] * h.quant_step;
            mlevel_1d_97_inv(dc, (int)Lp, h.levels, dt);
            for (size_t i = 0; i < h.original_len; i++) o[i] = (uint8_t)clamp_byte(dc[i]);
            free(dc); free(dt);
        }
        if (h.mode == MODE_LOSSLESS && crc32_calc(o, h.original_len) != h.crc) st2 = WV_ERR_CRC;
        if (st2 != WV_OK) { free(o); free(coeffs); return st2; }
        *out = o; *out_len = h.original_len;
    } else {
        /* 2D */
        int levels = h.levels, channels = h.channels;
        size_t block = (size_t)1 << levels;
        int Wp = (int)round_up(h.width, block);
        int Hp = (int)round_up(h.height, block);
        size_t plane = (size_t)Wp * Hp;
        size_t maxdim = (size_t)(Wp > Hp ? Wp : Hp);
        size_t total = (size_t)h.width * h.height * channels;
        uint8_t *o = (uint8_t *)malloc(total ? total : 1);
        if (!o) { free(coeffs); return WV_ERR_OOM; }

        if (h.mode == MODE_LOSSLESS) {
            int64_t *P  = (int64_t *)malloc(plane * sizeof(int64_t));
            int64_t *rt = (int64_t *)malloc(maxdim * sizeof(int64_t));
            int64_t *col= (int64_t *)malloc(maxdim * sizeof(int64_t));
            int64_t *ct = (int64_t *)malloc(maxdim * sizeof(int64_t));
            if (!P||!rt||!col||!ct){free(P);free(rt);free(col);free(ct);free(o);free(coeffs);return WV_ERR_OOM;}
            for (int ch = 0; ch < channels; ch++) {
                memcpy(P, coeffs + (size_t)ch * plane, plane * sizeof(int64_t));
                mlevel_2d_53_inv(P, Wp, Hp, levels, rt, col, ct);
                for (uint32_t y = 0; y < h.height; y++)
                    for (uint32_t x = 0; x < h.width; x++) {
                        int64_t v = P[(size_t)y * Wp + x];
                        o[((size_t)y * h.width + x) * channels + ch] =
                            (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
                    }
            }
            free(P); free(rt); free(col); free(ct);
        } else {
            double *P  = (double *)malloc(plane * sizeof(double));
            double *rt = (double *)malloc(maxdim * sizeof(double));
            double *col= (double *)malloc(maxdim * sizeof(double));
            double *ct = (double *)malloc(maxdim * sizeof(double));
            if (!P||!rt||!col||!ct){free(P);free(rt);free(col);free(ct);free(o);free(coeffs);return WV_ERR_OOM;}
            for (int ch = 0; ch < channels; ch++) {
                for (size_t i = 0; i < plane; i++)
                    P[i] = (double)coeffs[(size_t)ch * plane + i] * h.quant_step;
                mlevel_2d_97_inv(P, Wp, Hp, levels, rt, col, ct);
                for (uint32_t y = 0; y < h.height; y++)
                    for (uint32_t x = 0; x < h.width; x++)
                        o[((size_t)y * h.width + x) * channels + ch] =
                            (uint8_t)clamp_byte(P[(size_t)y * Wp + x]);
            }
            free(P); free(rt); free(col); free(ct);
        }
        if (h.mode == MODE_LOSSLESS && crc32_calc(o, total) != h.crc) { free(o); free(coeffs); return WV_ERR_CRC; }
        *out = o; *out_len = total;
    }
    free(coeffs);
    return WV_OK;
}

/* ---------------------------------------------------------------------- */
/* Public: inspect / misc                                                  */
/* ---------------------------------------------------------------------- */

WV_API int wv_inspect(const uint8_t *blob, size_t len, wv_info *out) {
    if (!blob || !out) return WV_ERR_ARG;
    hdr h; int ver;
    int st = read_header(blob, len, &h, &ver);
    if (st != WV_OK) return st;
    out->version      = ver;
    out->mode         = h.mode;
    out->wavelet      = h.wavelet;
    out->levels       = h.levels;
    out->ndim         = h.ndim;
    out->channels     = h.channels;
    out->width        = h.width;
    out->height       = h.height;
    out->original_len = h.original_len;
    out->quant_step   = h.quant_step;
    out->crc32        = h.crc;
    return WV_OK;
}

WV_API const char *wv_version(void) { return WV_VERSION_STR; }

WV_API const char *wv_strerror(int status) {
    switch (status) {
        case WV_OK:          return "ok";
        case WV_ERR_ARG:     return "invalid argument";
        case WV_ERR_MAGIC:   return "not a WVLC container";
        case WV_ERR_VERSION: return "unsupported container version";
        case WV_ERR_CORRUPT: return "corrupt or truncated container";
        case WV_ERR_CRC:     return "CRC mismatch (data integrity failure)";
        case WV_ERR_OOM:     return "out of memory";
        default:             return "unknown error";
    }
}

WV_API void wv_free(void *ptr) { free(ptr); }
