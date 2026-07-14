/* png.c -- see png.h. Self-contained DEFLATE inflate + PNG decode. */
#include "png.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* DEFLATE (RFC 1951) inflate -- puff-style, decodes into a sized buffer   */
/* ====================================================================== */

typedef struct {
    const uint8_t *in;
    size_t        inlen;
    size_t        inpos;
    int           bitbuf;
    int           bitcnt;
    uint8_t      *out;
    size_t        outcap;
    size_t        outpos;
} inflator;

typedef struct {
    short counts[16];
    short symbols[288];
} huff;

static int getbit(inflator *s) {
    if (s->bitcnt == 0) {
        if (s->inpos >= s->inlen) return -1;
        s->bitbuf = s->in[s->inpos++];
        s->bitcnt = 8;
    }
    int b = s->bitbuf & 1;
    s->bitbuf >>= 1;
    s->bitcnt--;
    return b;
}

static int getbits(inflator *s, int need) {
    int val = 0;
    for (int i = 0; i < need; i++) {
        int b = getbit(s);
        if (b < 0) return -1;
        val |= b << i;
    }
    return val;
}

static int decode_sym(inflator *s, const huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = getbit(s);
        if (b < 0) return -1;
        code |= b;
        int count = h->counts[len];
        if (code - first < count) return h->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static void build_huff(huff *h, const uint8_t *lengths, int n) {
    for (int i = 0; i < 16; i++) h->counts[i] = 0;
    for (int i = 0; i < n; i++) h->counts[lengths[i]]++;
    h->counts[0] = 0;
    short offs[16];
    offs[0] = offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->counts[len];
    for (int sym = 0; sym < n; sym++)
        if (lengths[sym]) h->symbols[offs[lengths[sym]]++] = (short)sym;
}

static int out_byte(inflator *s, uint8_t v) {
    if (s->outpos >= s->outcap) return -1;
    s->out[s->outpos++] = v;
    return 0;
}

static const short LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,
                                51,59,67,83,99,115,131,163,195,227,258};
static const short LEXT[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,
                                5,5,5,5,0};
static const short DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,
                                385,513,769,1025,1537,2049,3073,4097,6145,8193,
                                12289,16385,24577};
static const short DEXT[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,
                                10,11,11,12,12,13,13};

static int inflate_block(inflator *s, const huff *lit, const huff *dist) {
    for (;;) {
        int sym = decode_sym(s, lit);
        if (sym < 0) return -1;
        if (sym == 256) return 0;                       /* end of block */
        if (sym < 256) {
            if (out_byte(s, (uint8_t)sym) < 0) return -1;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int extra = getbits(s, LEXT[sym]);
            if (extra < 0) return -1;
            int length = LBASE[sym] + extra;
            int dsym = decode_sym(s, dist);
            if (dsym < 0 || dsym >= 30) return -1;
            int dextra = getbits(s, DEXT[dsym]);
            if (dextra < 0) return -1;
            int d = DBASE[dsym] + dextra;
            if ((size_t)d > s->outpos) return -1;
            for (int i = 0; i < length; i++) {
                if (out_byte(s, s->out[s->outpos - d]) < 0) return -1;
            }
        }
    }
}

static void fixed_huffs(huff *lit, huff *dist) {
    uint8_t l[288], d[30];
    for (int i = 0; i < 144; i++) l[i] = 8;
    for (int i = 144; i < 256; i++) l[i] = 9;
    for (int i = 256; i < 280; i++) l[i] = 7;
    for (int i = 280; i < 288; i++) l[i] = 8;
    for (int i = 0; i < 30; i++) d[i] = 5;
    build_huff(lit, l, 288);
    build_huff(dist, d, 30);
}

static int dynamic_huffs(inflator *s, huff *lit, huff *dist) {
    static const int order[19] =
        {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int hlit = getbits(s, 5);  if (hlit < 0) return -1; hlit += 257;
    int hdist = getbits(s, 5); if (hdist < 0) return -1; hdist += 1;
    int hclen = getbits(s, 4); if (hclen < 0) return -1; hclen += 4;
    if (hlit > 286 || hdist > 30) return -1;

    uint8_t clen[19];
    memset(clen, 0, sizeof clen);
    for (int i = 0; i < hclen; i++) {
        int v = getbits(s, 3);
        if (v < 0) return -1;
        clen[order[i]] = (uint8_t)v;
    }
    huff clh;
    build_huff(&clh, clen, 19);

    uint8_t lengths[286 + 30];
    int n = 0, total = hlit + hdist;
    while (n < total) {
        int sym = decode_sym(s, &clh);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (n == 0) return -1;
            int r = getbits(s, 2); if (r < 0) return -1; r += 3;
            uint8_t prev = lengths[n - 1];
            while (r-- && n < total) lengths[n++] = prev;
        } else if (sym == 17) {
            int r = getbits(s, 3); if (r < 0) return -1; r += 3;
            while (r-- && n < total) lengths[n++] = 0;
        } else { /* 18 */
            int r = getbits(s, 7); if (r < 0) return -1; r += 11;
            while (r-- && n < total) lengths[n++] = 0;
        }
    }
    build_huff(lit, lengths, hlit);
    build_huff(dist, lengths + hlit, hdist);
    return 0;
}

/* Inflate a raw DEFLATE stream into out (exact capacity). Returns bytes or -1. */
static long inflate_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap) {
    inflator s;
    s.in = in; s.inlen = inlen; s.inpos = 0;
    s.bitbuf = 0; s.bitcnt = 0;
    s.out = out; s.outcap = outcap; s.outpos = 0;

    int final = 0;
    while (!final) {
        final = getbit(&s);
        if (final < 0) return -1;
        int type = getbits(&s, 2);
        if (type < 0) return -1;
        if (type == 0) {                          /* stored */
            s.bitbuf = 0; s.bitcnt = 0;           /* align to byte */
            if (s.inpos + 4 > s.inlen) return -1;
            int len = s.in[s.inpos] | (s.in[s.inpos + 1] << 8);
            s.inpos += 4;                         /* skip LEN + NLEN */
            if (s.inpos + (size_t)len > s.inlen) return -1;
            for (int i = 0; i < len; i++)
                if (out_byte(&s, s.in[s.inpos++]) < 0) return -1;
        } else if (type == 1) {                   /* fixed */
            huff lit, dist; fixed_huffs(&lit, &dist);
            if (inflate_block(&s, &lit, &dist) < 0) return -1;
        } else if (type == 2) {                   /* dynamic */
            huff lit, dist;
            if (dynamic_huffs(&s, &lit, &dist) < 0) return -1;
            if (inflate_block(&s, &lit, &dist) < 0) return -1;
        } else {
            return -1;
        }
    }
    return (long)s.outpos;
}

/* ====================================================================== */
/* PNG                                                                     */
/* ====================================================================== */

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

int png_decode(const uint8_t *data, size_t n, Image *out) {
    static const uint8_t SIG[8] = {137,80,78,71,13,10,26,10};
    if (n < 8 || memcmp(data, SIG, 8) != 0) return 0;

    uint32_t width = 0, height = 0;
    int bitdepth = 0, colortype = -1, interlace = 0;
    uint8_t palette[256 * 3];
    uint8_t palalpha[256];
    int have_pal = 0, pal_n = 0, have_trns = 0;
    for (int i = 0; i < 256; i++) palalpha[i] = 255;

    /* collect IDAT */
    uint8_t *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;

    size_t pos = 8;
    int saw_iend = 0;
    while (pos + 8 <= n) {
        uint32_t clen = be32(data + pos);
        const uint8_t *ctype = data + pos + 4;
        const uint8_t *cdata = data + pos + 8;
        if (pos + 12 + clen > n) break;

        if (memcmp(ctype, "IHDR", 4) == 0 && clen >= 13) {
            width = be32(cdata);
            height = be32(cdata + 4);
            bitdepth = cdata[8];
            colortype = cdata[9];
            interlace = cdata[12];
        } else if (memcmp(ctype, "PLTE", 4) == 0) {
            pal_n = (int)(clen / 3);
            if (pal_n > 256) pal_n = 256;
            memcpy(palette, cdata, (size_t)pal_n * 3);
            have_pal = 1;
        } else if (memcmp(ctype, "tRNS", 4) == 0) {
            if (colortype == 3) {
                int m = (int)clen; if (m > 256) m = 256;
                memcpy(palalpha, cdata, m);
                have_trns = 1;
            }
        } else if (memcmp(ctype, "IDAT", 4) == 0) {
            if (idat_len + clen > idat_cap) {
                size_t ncap = idat_cap ? idat_cap * 2 : 4096;
                while (ncap < idat_len + clen) ncap *= 2;
                uint8_t *np = (uint8_t *)realloc(idat, ncap);
                if (!np) { free(idat); return 0; }
                idat = np; idat_cap = ncap;
            }
            memcpy(idat + idat_len, cdata, clen);
            idat_len += clen;
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            saw_iend = 1;
            break;
        }
        pos += 12 + clen;
    }
    (void)saw_iend; (void)have_pal;

    if (colortype < 0 || width == 0 || height == 0 || !idat) { free(idat); return 0; }
    if (bitdepth != 8 || interlace != 0) { free(idat); return 0; }
    if (width > 30000 || height > 30000) { free(idat); return 0; }

    int chans;
    switch (colortype) {
        case 0: chans = 1; break;   /* gray        */
        case 2: chans = 3; break;   /* rgb         */
        case 3: chans = 1; break;   /* palette idx */
        case 4: chans = 2; break;   /* gray+alpha  */
        case 6: chans = 4; break;   /* rgba        */
        default: free(idat); return 0;
    }

    size_t stride = 1 + (size_t)width * chans;
    size_t raw_size = stride * height;
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    if (!raw) { free(idat); return 0; }

    /* zlib wrapper: 2-byte header, then raw DEFLATE, then adler32 (ignored) */
    if (idat_len < 2) { free(idat); free(raw); return 0; }
    long got = inflate_raw(idat + 2, idat_len - 2, raw, raw_size);
    free(idat);
    if (got != (long)raw_size) { free(raw); return 0; }

    /* unfilter in place */
    int bpp = chans; /* bytes per pixel (8-bit samples) */
    for (uint32_t y = 0; y < height; y++) {
        uint8_t *row = raw + (size_t)y * stride;
        int ft = row[0];
        uint8_t *cur = row + 1;
        uint8_t *prev = (y > 0) ? raw + (size_t)(y - 1) * stride + 1 : NULL;
        size_t rb = (size_t)width * chans;
        for (size_t i = 0; i < rb; i++) {
            int a = (i >= (size_t)bpp) ? cur[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0;
            int x = cur[i];
            switch (ft) {
                case 0: break;
                case 1: x += a; break;
                case 2: x += b; break;
                case 3: x += (a + b) >> 1; break;
                case 4: x += paeth(a, b, c); break;
                default: free(raw); return 0;
            }
            cur[i] = (uint8_t)x;
        }
    }

    /* convert to Image (gray types -> 1 channel, color -> 3, alpha over white) */
    int och = (colortype == 0 || colortype == 4) ? 1 : 3;
    uint8_t *px = (uint8_t *)malloc((size_t)width * height * och);
    if (!px) { free(raw); return 0; }

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *cur = raw + (size_t)y * stride + 1;
        uint8_t *dst = px + (size_t)y * width * och;
        for (uint32_t x = 0; x < width; x++) {
            if (colortype == 0) {
                dst[x] = cur[x];
            } else if (colortype == 4) {
                int g = cur[x*2], al = cur[x*2+1];
                dst[x] = (uint8_t)((g * al + 255 * (255 - al)) / 255);
            } else if (colortype == 2) {
                dst[x*3+0] = cur[x*3+0]; dst[x*3+1] = cur[x*3+1]; dst[x*3+2] = cur[x*3+2];
            } else if (colortype == 6) {
                int r = cur[x*4+0], g = cur[x*4+1], b = cur[x*4+2], al = cur[x*4+3];
                dst[x*3+0] = (uint8_t)((r*al + 255*(255-al))/255);
                dst[x*3+1] = (uint8_t)((g*al + 255*(255-al))/255);
                dst[x*3+2] = (uint8_t)((b*al + 255*(255-al))/255);
            } else { /* palette */
                int idx = cur[x];
                int r = palette[idx*3+0], g = palette[idx*3+1], b = palette[idx*3+2];
                int al = have_trns ? palalpha[idx] : 255;
                dst[x*3+0] = (uint8_t)((r*al + 255*(255-al))/255);
                dst[x*3+1] = (uint8_t)((g*al + 255*(255-al))/255);
                dst[x*3+2] = (uint8_t)((b*al + 255*(255-al))/255);
            }
        }
    }
    free(raw);

    out->w = (int)width; out->h = (int)height; out->channels = och; out->px = px;
    return 1;
}
