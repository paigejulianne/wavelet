# wavelet — native C library + CLI

A portable C99 port of the wavelet compressor. Builds a **shared library**
(`wavelet.dll` on Windows, `libwavelet.so` on Linux, `libwavelet.dylib` on
macOS) and a **CLI** that links against it. Zero third-party dependencies —
only the C standard library.

```
data -> pad -> multi-level DWT (1D or separable 2D)
     -> quantize -> zigzag/varint -> order-0 range coder -> .wvlc container
```

- **5/3** Le Gall integer lifting → **lossless** (bit-exact, CRC-verified).
- **9/7** CDF float lifting → **lossy** (`--quality 1..100`).
- **1D** byte streams and **2D** images (separable row/column transform).
- Binary **PGM (P5)** and **PPM (P6)** images are auto-detected by the CLI.
- Never expands: incompressible input falls back to a stored container
  (only a fixed 56-byte header of overhead).

## Layout

```
include/wavelet.h   public C API (extern "C", export macros)
src/wavelet.c       transforms, range coder (Fenwick-tree model), CRC, container
src/cli.c           command-line front-end (+ netpbm I/O)
gui/main.c          Win32 GUI (compress / decompress / view images)
gui/image_io.c      BMP + PGM/PPM load/save for the GUI
gui/png.c           dependency-free PNG decoder (inflate + unfilter)
gui/app.rc          icon + manifest resources (embedded in the exes)
gui/app.manifest    Common Controls v6 (themed UI) + Per-Monitor-v2 DPI
assets/logo.png     app logo (512x512 RGBA)
assets/logo.ico     multi-size app icon (16..256)
tools/make_logo.py  regenerates the logo assets (stdlib only)
test/test.c         in-process self-test
CMakeLists.txt      cross-platform build
build.bat           Windows one-shot build (MSVC, no CMake needed)
build.sh            Linux/macOS one-shot build (gcc/clang)
```

## Building

### CMake (any platform)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build            # runs the self-test
```

Products land in `build/`: the shared library, the `wavelet` CLI, and
`wavelet_test`.

### Windows without CMake

```cmd
.\build.bat        # uses the installed Visual Studio (MSVC) toolchain
```
→ `build\wavelet.dll`, `build\wavelet.lib`, `build\wavelet.exe`, `build\wavelet_test.exe`

### Linux / macOS without CMake

```sh
./build.sh         # uses cc/gcc/clang
```
→ `build/libwavelet.so` (or `.dylib`), `build/wavelet`, `build/wavelet_test`

### Cross-compiling a Linux .so from Windows (MinGW)

```sh
x86_64-linux-gnu-gcc -O2 -std=c99 -fPIC -shared -DWV_BUILD_SHARED \
    -Iinclude src/wavelet.c -o libwavelet.so -lm
```

## CLI usage

```bash
# Lossless (default) — 1D stream or auto-detected PGM/PPM image
wavelet compress   input.dat  output.wvlc
wavelet decompress output.wvlc restored.dat

# Lossy, quality 1..100
wavelet compress photo.ppm photo.wvlc --quality 85
wavelet decompress photo.wvlc photo.out.ppm

# Force a wavelet / depth, or treat a netpbm file as raw bytes
wavelet compress in out --wavelet 97 --levels 5
wavelet compress in.pgm out --raw

# Inspect a container
wavelet info output.wvlc
```

## Windows GUI — *Wavelet Image Studio*

A native Win32 + GDI front-end (`wavelet_gui.exe`, built on Windows only) for
compressing, decompressing, and viewing wavelet images.

- **Open Image…** — load a source `.png` (8-bit), `.bmp` (24/8-bit), `.pgm`,
  or `.ppm`.
- **Compress & Save…** — encode to `.wvlc`; the app then displays the
  *reconstruction* a viewer would see, with size, ratio, and RMSE.
- **Open .wvlc…** — decode and view a compressed wavelet image.
- **Save Image…** — export the current view to BMP / PGM / PPM.
- **Lossless** toggle and a **quality** slider (1–100) drive the codec
  (5/3 reversible vs 9/7 lossy). The image pane scales to fit with halftone
  filtering.
- **Split view** — show the original (left) and reconstruction (right)
  side by side, with the reconstruction's RMSE in its caption.
- **Drag & drop** — drop an image or a `.wvlc` onto the window to open it.
- The window and both executables carry the app icon (`assets/logo.ico`,
  embedded via `gui/app.rc`).
- The UI is **themed** (Common Controls v6) and **Per-Monitor-v2 DPI-aware**
  via `gui/app.manifest`: modern Segoe UI controls that stay crisp at any
  display scale, over a light command strip / dark image canvas.

PNG input is handled by a small self-contained decoder (`gui/png.c`:
DEFLATE inflate + all five scanline filters, grayscale / RGB / palette /
alpha, 8-bit non-interlaced) — no libpng/zlib dependency.

Regenerate the logo assets any time with `python tools/make_logo.py`.

Build it with `build.bat` (or CMake on Windows), then run `wavelet_gui.exe`.
It must sit next to `wavelet.dll`.

A headless pipeline check (load → compress → decompress → render → save,
no display required) runs via:

```
wavelet_gui.exe --selftest <image.bmp> <output_dir>
```

which writes a reconstruction BMP and a `selftest.log`, and returns non-zero
on any failure.

## C API

```c
#include "wavelet.h"

uint8_t *blob; size_t blob_len;
int rc = wv_compress(data, len, /*lossy=*/0, WV_WAVELET_AUTO,
                     /*levels=*/-1, /*quality=*/-1, &blob, &blob_len);

uint8_t *back; size_t back_len;
wv_decompress(blob, blob_len, &back, &back_len);

wv_free(blob);
wv_free(back);
```

Images use `wv_compress_image(pixels, w, h, channels, ...)`. Buffers returned
by the library are released with `wv_free()`. `wv_inspect()` reads container
metadata (mode, wavelet, geometry, CRC) without decompressing. All entry
points return a `wv_status` code; `wv_strerror()` renders it.

## Container format (`WVLC`, v1)

56-byte little-endian header followed by the range-coded payload:

| off | size | field                                             |
|-----|------|---------------------------------------------------|
| 0   | 4    | magic `"WVLC"`                                    |
| 4   | 1    | format version                                    |
| 5   | 1    | mode (0 stored-raw, 1 lossless, 2 lossy, 3 stored-rc) |
| 6   | 1    | wavelet (0 = 5/3, 1 = 9/7)                        |
| 7   | 1    | levels                                            |
| 8   | 1    | ndim (1 or 2)                                     |
| 9   | 1    | channels                                          |
| 10  | 2    | reserved                                          |
| 12  | 4    | CRC-32 of original                                |
| 16  | 4    | width   (2D)                                      |
| 20  | 4    | height  (2D)                                      |
| 24  | 8    | original length (bytes)                           |
| 32  | 8    | quantization step (f64, lossy)                    |
| 40  | 8    | coefficient count                                 |
| 48  | 8    | symbol-stream length                              |

## Notes

- The entropy stage is an order-0 adaptive range coder whose symbol model is
  backed by a **Fenwick (binary-indexed) tree**, so cumulative-sum and
  symbol-by-cumulative lookups are O(log 256) rather than O(256) per symbol.
- The `.wvlc` container is **not** interchangeable with the Python tool's
  `.wvlt` files: the C version uses a self-contained range coder instead of
  zlib, so it stays dependency-free on every platform.
- Lossless correctness is guaranteed by the reversible integer lifting and
  verified with a stored CRC-32 on every decompress.
