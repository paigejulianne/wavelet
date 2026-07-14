#!/usr/bin/env python3
"""
make_logo.py -- generate the Wavelet Image Studio logo (PNG + ICO).

Pure standard library (math, struct, zlib). Renders a high-res RGBA master
depicting a glowing Morlet-style wavelet over a faint recursive subband grid on
a rounded teal tile, then downscales to produce:

    assets/logo.png   (512x512, RGBA)
    assets/logo.ico   (16/24/32/48/64/128/256, 32-bit + a 256 PNG entry)
"""
import math
import os
import struct
import zlib

MASTER = 768


# ---------------------------------------------------------------------------
# rendering
# ---------------------------------------------------------------------------

def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def render_master(N):
    R = [0.0] * (N * N)
    G = [0.0] * (N * N)
    B = [0.0] * (N * N)

    # background: diagonal teal gradient + soft center glow
    top = (10, 32, 48)
    bot = (12, 74, 84)
    cx, cy = N * 0.5, N * 0.46
    for y in range(N):
        ty = y / (N - 1)
        for x in range(N):
            tx = x / (N - 1)
            t = 0.5 * ty + 0.5 * tx
            r = top[0] + (bot[0] - top[0]) * t
            g = top[1] + (bot[1] - top[1]) * t
            b = top[2] + (bot[2] - top[2]) * t
            # radial glow toward center
            d = math.hypot(x - cx, y - cy) / (N * 0.6)
            glow = math.exp(-d * d * 2.2) * 26.0
            i = y * N + x
            R[i] = r + glow * 0.2
            G[i] = g + glow
            B[i] = b + glow * 1.05

    # faint recursive subband (Mallat) grid: split at 1/2, then LL at 1/4, 1/8
    def vline(fx, y0, y1, a):
        X = fx * N
        x0 = int(X - 1); x1 = int(X + 1)
        for x in range(max(0, x0), min(N, x1 + 1)):
            cov = a * (1.0 - abs((x + 0.5) - X))
            if cov <= 0:
                continue
            for y in range(int(y0 * N), int(y1 * N)):
                i = y * N + x
                R[i] += (235 - R[i]) * cov
                G[i] += (245 - G[i]) * cov
                B[i] += (255 - B[i]) * cov

    def hline(fy, x0, x1, a):
        Y = fy * N
        y0 = int(Y - 1); y1 = int(Y + 1)
        for y in range(max(0, y0), min(N, y1 + 1)):
            cov = a * (1.0 - abs((y + 0.5) - Y))
            if cov <= 0:
                continue
            base = y * N
            for x in range(int(x0 * N), int(x1 * N)):
                i = base + x
                R[i] += (235 - R[i]) * cov
                G[i] += (245 - G[i]) * cov
                B[i] += (255 - B[i]) * cov

    ga = 0.10
    vline(0.5, 0.0, 1.0, ga); hline(0.5, 0.0, 1.0, ga)
    vline(0.25, 0.0, 0.5, ga); hline(0.25, 0.0, 0.5, ga)
    vline(0.125, 0.0, 0.25, ga); hline(0.125, 0.0, 0.25, ga)

    # wavelet curve: damped cosine, drawn by stamping soft disks (glow + core)
    glow_col = (60, 224, 210)
    core_col = (214, 255, 250)

    def stamp(px, py, rad, col, strength):
        x0 = int(px - rad); x1 = int(px + rad)
        y0 = int(py - rad); y1 = int(py + rad)
        r2 = rad * rad
        for yy in range(max(0, y0), min(N, y1 + 1)):
            for xx in range(max(0, x0), min(N, x1 + 1)):
                dx = xx + 0.5 - px; dy = yy + 0.5 - py
                dd = dx * dx + dy * dy
                if dd > r2:
                    continue
                f = (1.0 - dd / r2)
                f = f * f * strength
                i = yy * N + xx
                R[i] = clamp(R[i] + col[0] * f, 0, 255)
                G[i] = clamp(G[i] + col[1] * f, 0, 255)
                B[i] = clamp(B[i] + col[2] * f, 0, 255)

    steps = 5000
    cxx = N * 0.5
    amp = N * 0.26
    sigma = N * 0.20
    freq = 4.2
    glow_r = N * 0.045
    core_r = N * 0.011
    x0f, x1f = N * 0.10, N * 0.90
    for s in range(steps + 1):
        x = x0f + (x1f - x0f) * s / steps
        env = math.exp(-((x - cxx) ** 2) / (2 * sigma * sigma))
        y = N * 0.5 - amp * env * math.cos((x - cxx) / (N * 0.5) * freq * math.pi)
        stamp(x, y, glow_r, glow_col, 0.045)
    for s in range(steps + 1):
        x = x0f + (x1f - x0f) * s / steps
        env = math.exp(-((x - cxx) ** 2) / (2 * sigma * sigma))
        y = N * 0.5 - amp * env * math.cos((x - cxx) / (N * 0.5) * freq * math.pi)
        stamp(x, y, core_r, core_col, 0.9)

    # rounded-rect alpha mask (full-bleed tile with rounded corners).
    # Signed distance to a rounded box: length(max(q,0)) + min(max(qx,qy),0) - r
    A = [0.0] * (N * N)
    pad = N * 0.045
    rad = N * 0.205
    left, top_, right, bottom = pad, pad, N - pad, N - pad
    cx0 = (left + right) * 0.5
    cy0 = (top_ + bottom) * 0.5
    hx = (right - left) * 0.5 - rad
    hy = (bottom - top_) * 0.5 - rad
    for y in range(N):
        for x in range(N):
            qx = abs(x + 0.5 - cx0) - hx
            qy = abs(y + 0.5 - cy0) - hy
            dist = math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - rad
            A[y * N + x] = clamp(0.5 - dist, 0.0, 1.0)

    out = bytearray(N * N * 4)
    for i in range(N * N):
        out[i*4+0] = int(clamp(R[i], 0, 255))
        out[i*4+1] = int(clamp(G[i], 0, 255))
        out[i*4+2] = int(clamp(B[i], 0, 255))
        out[i*4+3] = int(A[i] * 255)
    return out, N


# ---------------------------------------------------------------------------
# resize (area average, premultiplied alpha)
# ---------------------------------------------------------------------------

def resize(src, sw, sh, dw, dh):
    dst = bytearray(dw * dh * 4)
    sxr = sw / dw
    syr = sh / dh
    for dy in range(dh):
        sy0 = dy * syr; sy1 = (dy + 1) * syr
        iy0 = int(sy0); iy1 = min(sh, int(math.ceil(sy1)))
        for dx in range(dw):
            sx0 = dx * sxr; sx1 = (dx + 1) * sxr
            ix0 = int(sx0); ix1 = min(sw, int(math.ceil(sx1)))
            ar = ag = ab = aa = wsum = 0.0
            for yy in range(iy0, iy1):
                wy = min(sy1, yy + 1) - max(sy0, yy)
                for xx in range(ix0, ix1):
                    wx = min(sx1, xx + 1) - max(sx0, xx)
                    w = wx * wy
                    if w <= 0:
                        continue
                    si = (yy * sw + xx) * 4
                    a = src[si+3] / 255.0
                    ar += src[si+0] * a * w
                    ag += src[si+1] * a * w
                    ab += src[si+2] * a * w
                    aa += src[si+3] * w
                    wsum += w
            di = (dy * dw + dx) * 4
            if wsum > 0 and aa > 0:
                ap = aa / wsum            # 0..255
                pa = (aa) / (255.0 * wsum)  # premult weight
                dst[di+0] = int(clamp(ar / (pa * wsum), 0, 255)) if pa > 0 else 0
                dst[di+1] = int(clamp(ag / (pa * wsum), 0, 255)) if pa > 0 else 0
                dst[di+2] = int(clamp(ab / (pa * wsum), 0, 255)) if pa > 0 else 0
                dst[di+3] = int(clamp(ap, 0, 255))
            else:
                dst[di+0] = dst[di+1] = dst[di+2] = dst[di+3] = 0
    return dst


# ---------------------------------------------------------------------------
# PNG / ICO writers
# ---------------------------------------------------------------------------

def write_png_bytes(rgba, w, h):
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter: None
        raw += rgba[y*w*4:(y+1)*w*4]
    comp = zlib.compress(bytes(raw), 9)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", comp) + chunk(b"IEND", b""))


def dib_bytes(rgba, w, h):
    """32-bit BGRA bottom-up DIB with doubled height + AND mask (all opaque)."""
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    body = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            si = (y * w + x) * 4
            body += bytes((rgba[si+2], rgba[si+1], rgba[si+0], rgba[si+3]))
    mask_row = ((w + 31) // 32) * 4
    andmask = bytes(mask_row * h)
    return hdr + bytes(body) + andmask


def write_ico(path, master, msize):
    sizes = [16, 24, 32, 48, 64, 128, 256]
    entries = []
    for s in sizes:
        img = resize(master, msize, msize, s, s)
        if s == 256:
            data = write_png_bytes(img, s, s)      # PNG-compressed entry
        else:
            data = dib_bytes(img, s, s)            # BMP DIB entry
        entries.append((s, data))

    out = bytearray()
    out += struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    for s, data in entries:
        bw = 0 if s == 256 else s
        bh = 0 if s == 256 else s
        out += struct.pack("<BBBBHHII", bw, bh, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in entries:
        out += data
    with open(path, "wb") as f:
        f.write(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    assets = os.path.join(here, "..", "assets")
    os.makedirs(assets, exist_ok=True)

    print("rendering %dx%d master..." % (MASTER, MASTER))
    master, N = render_master(MASTER)

    print("writing logo.png (512x512)...")
    png512 = resize(master, N, N, 512, 512)
    with open(os.path.join(assets, "logo.png"), "wb") as f:
        f.write(write_png_bytes(png512, 512, 512))

    print("writing logo.ico...")
    write_ico(os.path.join(assets, "logo.ico"), master, N)

    print("done:", os.path.normpath(os.path.join(assets, "logo.png")),
          "and logo.ico")


if __name__ == "__main__":
    main()
