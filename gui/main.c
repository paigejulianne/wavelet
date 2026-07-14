/*
 * main.c -- Win32 GUI for the wavelet image compressor.
 *
 * Load a source image (BMP / PGM / PPM), compress it to a .wvlc container with
 * the wavelet library, preview the reconstruction with size + RMSE stats, open
 * and view existing .wvlc images, and export the viewed image back to an image
 * file. Links against wavelet.dll.
 *
 * The UI is themed (Common Controls v6 via the embedded manifest), DPI-aware
 * (every metric is scaled from a 96-dpi baseline through the S() helper), and
 * rendered with the Segoe UI font over a cohesive light-toolbar / dark-canvas
 * layout.
 *
 * A headless "--selftest <image> <outdir>" path exercises the whole
 * load -> compress -> decompress -> render pipeline for CI without a display.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "wavelet.h"
#include "image_io.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

#define ID_OPEN_IMG   1001
#define ID_OPEN_WVLC  1002
#define ID_COMPRESS   1003
#define ID_SAVE_IMG   1004
#define ID_LOSSLESS   1005
#define ID_QUALITY    1006
#define ID_SPLIT      1007
#define IDI_APPICON   101

/* WM_DPICHANGED predates some SDKs we might build against. */
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

/* ---------------- theme ---------------- */
/* A cohesive modern palette: a light Windows-11-style command strip on top,
 * a neutral-dark image canvas below (the standard image-viewer aesthetic). */
#define COL_TOOLBAR   RGB(247, 248, 250)
#define COL_TOOLBAR_B RGB(220, 223, 228)   /* toolbar / status hairline border */
#define COL_TEXT      RGB(32,  34,  38)
#define COL_TEXT_DIM  RGB(96, 100, 108)
#define COL_CANVAS    RGB(30,  32,  36)
#define COL_CAPTION   RGB(22,  24,  28)
#define COL_CAPTION_T RGB(222, 226, 232)
#define COL_HINT      RGB(150, 155, 162)

static HWND g_main;
static HWND g_btnOpenImg, g_btnOpenWvlc, g_btnCompress, g_btnSave;
static HWND g_track, g_qval, g_chkLossless, g_chkSplit;
static HFONT g_font;
static HBRUSH g_brToolbar;
static UINT   g_dpi = 96;
static int    g_toolH = 46, g_statH = 26;   /* recomputed per-DPI */
static char   g_statusText[600] = "Ready.";

static Image  g_source;       /* original / source image (for compression + RMSE) */
static Image  g_view;         /* currently displayed reconstruction / image        */
static int    g_quality = 80;
static int    g_have_recon = 0;   /* g_view is a reconstruction distinct from source */
static double g_last_rmse = -1.0;

/* Scale a 96-dpi design metric to the current DPI. */
#define S(x) MulDiv((x), (int)g_dpi, 96)

/* ---------------- DPI helpers ---------------- */

/* GetDpiForWindow (Win10 1607+) resolved dynamically so we still build/run on
 * older toolchains and systems; falls back to the device-context DPI. */
static UINT win_dpi(HWND h) {
    typedef UINT (WINAPI *GetDpiForWindow_t)(HWND);
    static GetDpiForWindow_t fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE u = GetModuleHandleA("user32.dll");
        if (u) fn = (GetDpiForWindow_t)GetProcAddress(u, "GetDpiForWindow");
    }
    if (fn) { UINT d = fn(h); if (d) return d; }
    HDC dc = GetDC(h);
    UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(h, dc);
    return d ? d : 96;
}

static void make_font(void) {
    if (g_font) DeleteObject(g_font);
    LOGFONTA lf;
    ZeroMemory(&lf, sizeof lf);
    lf.lfHeight = -MulDiv(10, (int)g_dpi, 72);   /* 10pt Segoe UI */
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyA(lf.lfFaceName, "Segoe UI");
    g_font = CreateFontIndirectA(&lf);
    if (!g_font) g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static void set_ctl_font(HWND c) {
    if (c) SendMessageA(c, WM_SETFONT, (WPARAM)g_font, TRUE);
}
static void apply_font_all(void) {
    set_ctl_font(g_btnOpenImg);  set_ctl_font(g_btnOpenWvlc);
    set_ctl_font(g_btnCompress); set_ctl_font(g_btnSave);
    set_ctl_font(g_chkLossless); set_ctl_font(g_track);
    set_ctl_font(g_qval);        set_ctl_font(g_chkSplit);
}

/* ---------------- utilities ---------------- */

static void set_status(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_statusText, sizeof g_statusText, fmt, ap);
    va_end(ap);
    if (g_main) {
        RECT rc; GetClientRect(g_main, &rc);
        RECT st = { 0, rc.bottom - g_statH, rc.right, rc.bottom };
        InvalidateRect(g_main, &st, FALSE);
    }
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!b) { fclose(f); return NULL; }
    *len = fread(b, 1, (size_t)sz, f);
    fclose(f);
    return b;
}
static int write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t put = fwrite(data, 1, len, f);
    fclose(f);
    return put == len;
}

static double image_rmse(const Image *a, const Image *b) {
    if (a->w != b->w || a->h != b->h || a->channels != b->channels) return -1.0;
    size_t n = (size_t)a->w * a->h * a->channels;
    double s = 0;
    for (size_t i = 0; i < n; i++) { double d = (double)a->px[i] - b->px[i]; s += d*d; }
    return sqrt(s / (double)n);
}

static int is_lossless(void) {
    return SendMessageA(g_chkLossless, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void update_quality_label(void) {
    char b[32];
    if (is_lossless()) strcpy(b, "Lossless");
    else sprintf(b, "Quality %d", g_quality);
    SetWindowTextA(g_qval, b);
}

/* Set the displayed image to a copy-free owned buffer. */
static void set_view(Image *img /* moved-from */) {
    img_free(&g_view);
    g_view = *img;
    memset(img, 0, sizeof *img);
    InvalidateRect(g_main, NULL, TRUE);
}

/* ---------------- file dialogs ---------------- */

static int dlg_open(HWND h, const char *filter, char *out, size_t n) {
    OPENFILENAMEA ofn; ZeroMemory(&ofn, sizeof ofn);
    out[0] = 0;
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = h;
    ofn.lpstrFilter = filter; ofn.lpstrFile = out; ofn.nMaxFile = (DWORD)n;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    return GetOpenFileNameA(&ofn);
}
static int dlg_save(HWND h, const char *filter, const char *defext, char *out, size_t n) {
    OPENFILENAMEA ofn; ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = h;
    ofn.lpstrFilter = filter; ofn.lpstrFile = out; ofn.nMaxFile = (DWORD)n;
    ofn.lpstrDefExt = defext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    return GetSaveFileNameA(&ofn);
}

/* ---------------- actions ---------------- */

static void load_image_path(HWND h, const char *path) {
    Image im;
    if (!img_load(path, &im)) { MessageBoxA(h, "Unsupported or unreadable image.\n"
        "Supported: PNG (8-bit), 24/8-bit BMP, binary PGM (P5), binary PPM (P6).", "Open image", MB_ICONWARNING); return; }
    img_free(&g_source);
    img_copy(&g_source, &im);
    g_have_recon = 0; g_last_rmse = -1.0;
    set_view(&im);
    EnableWindow(g_btnCompress, TRUE);
    EnableWindow(g_btnSave, TRUE);
    const char *cn = g_source.channels == 1 ? "grayscale" : "RGB";
    set_status("Loaded  %dx%d  %s  (%d bytes)   \x95   set options, then Compress & Save",
               g_source.w, g_source.h, cn, g_source.w * g_source.h * g_source.channels);
}

static void action_open_image(HWND h) {
    char path[MAX_PATH];
    if (!dlg_open(h, "Images (*.png;*.bmp;*.pgm;*.ppm)\0*.png;*.bmp;*.pgm;*.ppm\0All files\0*.*\0", path, sizeof path))
        return;
    load_image_path(h, path);
}

static void action_compress(HWND h) {
    if (!g_source.px) { MessageBoxA(h, "Open a source image first.", "Compress", MB_ICONINFORMATION); return; }
    int lossy = !is_lossless();
    uint8_t *blob = NULL; size_t bl = 0;
    int rc = wv_compress_image(g_source.px, g_source.w, g_source.h, g_source.channels,
                               lossy, WV_WAVELET_AUTO, -1, lossy ? g_quality : -1, &blob, &bl);
    if (rc != WV_OK) { MessageBoxA(h, wv_strerror(rc), "Compress failed", MB_ICONERROR); return; }

    char path[MAX_PATH];
    if (!dlg_save(h, "Wavelet image (*.wvlc)\0*.wvlc\0All files\0*.*\0", "wvlc", path, sizeof path)) {
        wv_free(blob); return;
    }
    if (!write_file(path, blob, bl)) {
        MessageBoxA(h, "Could not write file.", "Compress", MB_ICONERROR); wv_free(blob); return;
    }

    /* Preview the reconstruction that a viewer would see. */
    uint8_t *raw = NULL; size_t rl = 0;
    wv_info info; wv_inspect(blob, bl, &info);
    Image recon; memset(&recon, 0, sizeof recon);
    double rmse = -1.0;
    if (wv_decompress(blob, bl, &raw, &rl) == WV_OK) {
        recon.w = (int)info.width; recon.h = (int)info.height; recon.channels = info.channels;
        recon.px = (uint8_t *)malloc(rl ? rl : 1);
        memcpy(recon.px, raw, rl);
        wv_free(raw);
        rmse = image_rmse(&g_source, &recon);
        g_last_rmse = rmse;
        g_have_recon = 1;
        set_view(&recon);
    }
    size_t orig = (size_t)g_source.w * g_source.h * g_source.channels;
    double pct = orig ? (double)bl * 100.0 / (double)orig : 0.0;
    double ratio = bl ? (double)orig / (double)bl : 0.0;
    if (rmse >= 0)
        set_status("Saved %s   %zu \x1a %zu bytes  (%.1f%%, %.1f:1)   %s   RMSE %.2f",
                   path, orig, bl, pct, ratio, lossy ? "lossy 9/7" : "lossless 5/3", rmse);
    else
        set_status("Saved %s   %zu \x1a %zu bytes  (%.1f%%, %.1f:1)   %s",
                   path, orig, bl, pct, ratio, lossy ? "lossy 9/7" : "lossless 5/3");
    wv_free(blob);
}

static void load_wvlc_path(HWND h, const char *path) {
    size_t bl; uint8_t *blob = read_file(path, &bl);
    if (!blob) { MessageBoxA(h, "Could not read file.", "Open", MB_ICONERROR); return; }

    wv_info info;
    int rc = wv_inspect(blob, bl, &info);
    if (rc != WV_OK) { MessageBoxA(h, wv_strerror(rc), "Open", MB_ICONERROR); free(blob); return; }
    if (info.ndim != 2) {
        MessageBoxA(h, "This container holds a 1D byte stream, not an image.", "Open", MB_ICONWARNING);
        free(blob); return;
    }
    uint8_t *raw = NULL; size_t rl = 0;
    rc = wv_decompress(blob, bl, &raw, &rl);
    free(blob);
    if (rc != WV_OK) { MessageBoxA(h, wv_strerror(rc), "Decompress failed", MB_ICONERROR); return; }

    Image im; im.w = (int)info.width; im.h = (int)info.height; im.channels = info.channels;
    im.px = (uint8_t *)malloc(rl ? rl : 1); memcpy(im.px, raw, rl); wv_free(raw);

    img_free(&g_source);
    img_copy(&g_source, &im);            /* allow re-save / re-compress */
    g_have_recon = 0; g_last_rmse = -1.0;
    set_view(&im);
    EnableWindow(g_btnSave, TRUE);
    EnableWindow(g_btnCompress, TRUE);
    const char *mode = info.mode == WV_MODE_LOSSY ? "lossy 9/7" :
                       info.mode == WV_MODE_LOSSLESS ? "lossless 5/3" : "stored";
    set_status("Opened %s   %dx%d  %s   %s   container %zu bytes",
               path, info.width, info.height,
               info.channels == 1 ? "grayscale" : "RGB", mode, bl);
}

static void action_open_wvlc(HWND h) {
    char path[MAX_PATH];
    if (!dlg_open(h, "Wavelet image (*.wvlc)\0*.wvlc\0All files\0*.*\0", path, sizeof path)) return;
    load_wvlc_path(h, path);
}

static void action_save_image(HWND h) {
    if (!g_view.px) { MessageBoxA(h, "Nothing to save.", "Save image", MB_ICONINFORMATION); return; }
    char path[MAX_PATH];
    if (!dlg_save(h, "BMP image (*.bmp)\0*.bmp\0PGM gray (*.pgm)\0*.pgm\0PPM color (*.ppm)\0*.ppm\0",
                  "bmp", path, sizeof path)) return;
    if (img_save(path, &g_view)) set_status("Saved image  %s  (%dx%d)", path, g_view.w, g_view.h);
    else MessageBoxA(h, "Could not save image.", "Save image", MB_ICONERROR);
}

/* ---------------- rendering ---------------- */

/* Blit an image scaled to fit inside `area` (centered, aspect-preserved). */
static void draw_image_in(HDC hdc, const Image *im, RECT area) {
    if (!im->px) return;
    int aw = area.right - area.left, ah = area.bottom - area.top;
    if (aw < 1 || ah < 1) return;
    double s = (double)aw / im->w;
    double sy = (double)ah / im->h;
    if (sy < s) s = sy;
    int dw = (int)(im->w * s), dh = (int)(im->h * s);
    if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    int dx = area.left + (aw - dw) / 2, dy = area.top + (ah - dh) / 2;

    uint8_t *bgr = img_to_bgr24(im);
    if (!bgr) return;
    BITMAPINFO bmi; ZeroMemory(&bmi, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = im->w;
    bmi.bmiHeader.biHeight = -im->h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);
    StretchDIBits(hdc, dx, dy, dw, dh, 0, 0, im->w, im->h,
                  bgr, &bmi, DIB_RGB_COLORS, SRCCOPY);
    free(bgr);
}

/* Caption bar at the top of a pane. */
static void draw_caption(HDC hdc, RECT pane, const char *text) {
    int barh = S(24);
    RECT bar = { pane.left, pane.top, pane.right, pane.top + barh };
    HBRUSH b = CreateSolidBrush(COL_CAPTION);
    FillRect(hdc, &bar, b);
    DeleteObject(b);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, COL_CAPTION_T);
    bar.left += S(10);
    DrawTextA(hdc, text, -1, &bar, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    HFONT old = (HFONT)SelectObject(hdc, g_font);

    /* command strip (top) */
    RECT tb = { 0, 0, rc.right, g_toolH };
    FillRect(hdc, &tb, g_brToolbar);
    RECT tbb = { 0, g_toolH - 1, rc.right, g_toolH };
    HBRUSH bd = CreateSolidBrush(COL_TOOLBAR_B);
    FillRect(hdc, &tbb, bd);

    /* image canvas (middle) */
    RECT area = { 0, g_toolH, rc.right, rc.bottom - g_statH };
    HBRUSH bg = CreateSolidBrush(COL_CANVAS);
    FillRect(hdc, &area, bg);
    DeleteObject(bg);

    int split = (SendMessageA(g_chkSplit, BM_GETCHECK, 0, 0) == BST_CHECKED)
                && g_source.px && g_view.px;

    if (split) {
        int barh = S(24);
        int mid = (area.left + area.right) / 2;
        RECT left  = { area.left, area.top, mid - 1, area.bottom };
        RECT right = { mid + 1, area.top, area.right, area.bottom };
        RECT limg = { left.left, left.top + barh, left.right, left.bottom };
        RECT rimg = { right.left, right.top + barh, right.right, right.bottom };
        draw_image_in(hdc, &g_source, limg);
        draw_image_in(hdc, &g_view, rimg);
        /* divider */
        RECT div = { mid - 1, area.top, mid + 1, area.bottom };
        FillRect(hdc, &div, bd);
        char cap[128];
        draw_caption(hdc, left, "Original");
        if (g_have_recon && g_last_rmse >= 0) sprintf(cap, "Reconstruction   RMSE %.2f", g_last_rmse);
        else strcpy(cap, "Reconstruction");
        draw_caption(hdc, right, cap);
    } else if (g_view.px) {
        draw_image_in(hdc, &g_view, area);
    } else {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, COL_HINT);
        const char *msg = "Open an image to compress, or open a .wvlc to view.   Drag & drop is supported.";
        DrawTextA(hdc, msg, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    /* status strip (bottom) */
    RECT st = { 0, rc.bottom - g_statH, rc.right, rc.bottom };
    FillRect(hdc, &st, g_brToolbar);
    RECT stb = { 0, st.top, rc.right, st.top + 1 };
    FillRect(hdc, &stb, bd);
    DeleteObject(bd);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, COL_TEXT_DIM);
    RECT stt = st; stt.left += S(12); stt.right -= S(12);
    DrawTextA(hdc, g_statusText, -1, &stt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    SelectObject(hdc, old);
    EndPaint(h, &ps);
}

/* ---------------- window ---------------- */

static HWND mk_button(HWND parent, const char *text, int id) {
    return CreateWindowA("BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                         0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, NULL, NULL);
}

/* Position the command-strip controls for the current DPI. */
static void relayout_toolbar(void) {
    int pad = S(10), gap = S(6);
    int by = S(9),  bh = S(28);      /* buttons */
    int cy = S(13), ch = S(22);      /* checkboxes / label */
    int x  = pad;

    MoveWindow(g_btnOpenImg,  x, by, S(112), bh, TRUE); x += S(112) + gap;
    MoveWindow(g_btnOpenWvlc, x, by, S(108), bh, TRUE); x += S(108) + gap;
    MoveWindow(g_btnCompress, x, by, S(150), bh, TRUE); x += S(150) + gap;
    MoveWindow(g_btnSave,     x, by, S(108), bh, TRUE); x += S(108) + S(16);

    MoveWindow(g_chkLossless, x, cy, S(88), ch, TRUE);  x += S(88)  + gap;
    MoveWindow(g_track,       x, S(11), S(150), S(26), TRUE); x += S(150) + gap;
    MoveWindow(g_qval,        x, cy, S(90), ch, TRUE);  x += S(90)  + S(12);
    MoveWindow(g_chkSplit,    x, cy, S(96), ch, TRUE);
}

static void create_controls(HWND h) {
    g_btnOpenImg  = mk_button(h, "Open Image\x85", ID_OPEN_IMG);
    g_btnOpenWvlc = mk_button(h, "Open .wvlc\x85", ID_OPEN_WVLC);
    g_btnCompress = mk_button(h, "Compress && Save\x85", ID_COMPRESS);
    g_btnSave     = mk_button(h, "Save Image\x85", ID_SAVE_IMG);

    g_chkLossless = CreateWindowA("BUTTON", "Lossless", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                  0, 0, 0, 0, h, (HMENU)ID_LOSSLESS, NULL, NULL);

    g_track = CreateWindowA(TRACKBAR_CLASS, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                            0, 0, 0, 0, h, (HMENU)ID_QUALITY, NULL, NULL);
    SendMessageA(g_track, TBM_SETRANGE, TRUE, MAKELONG(1, 100));
    SendMessageA(g_track, TBM_SETPOS, TRUE, g_quality);

    g_qval = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                           0, 0, 0, 0, h, NULL, NULL, NULL);

    g_chkSplit = CreateWindowA("BUTTON", "Split view", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                               0, 0, 0, 0, h, (HMENU)ID_SPLIT, NULL, NULL);

    apply_font_all();
    relayout_toolbar();
    EnableWindow(g_btnCompress, FALSE);
    EnableWindow(g_btnSave, FALSE);
    update_quality_label();
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_dpi = win_dpi(h);
        g_toolH = S(46); g_statH = S(26);
        make_font();
        g_brToolbar = CreateSolidBrush(COL_TOOLBAR);
        create_controls(h);
        DragAcceptFiles(h, TRUE);
        return 0;
    case WM_SIZE:
        relayout_toolbar();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        g_toolH = S(46); g_statH = S(26);
        make_font();
        apply_font_all();
        RECT *pr = (RECT *)lp;   /* suggested position + size for the new DPI */
        SetWindowPos(h, NULL, pr->left, pr->top, pr->right - pr->left, pr->bottom - pr->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        relayout_toolbar();
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }
    /* Blend the themed toolbar controls (checkboxes, trackbar, label) into the
     * light command strip instead of the default window-grey. */
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, COL_TOOLBAR);
        SetTextColor(dc, COL_TEXT);
        return (LRESULT)g_brToolbar;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        char path[MAX_PATH];
        if (DragQueryFileA(drop, 0, path, sizeof path)) {
            size_t n = strlen(path);
            if (n >= 5 && _stricmp(path + n - 5, ".wvlc") == 0) load_wvlc_path(h, path);
            else load_image_path(h, path);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_ERASEBKGND:  return 1;                 /* painted in WM_PAINT */
    case WM_PAINT:       paint(h); return 0;
    case WM_HSCROLL:
        if ((HWND)lp == g_track) {
            g_quality = (int)SendMessageA(g_track, TBM_GETPOS, 0, 0);
            update_quality_label();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_OPEN_IMG:  action_open_image(h); break;
        case ID_OPEN_WVLC: action_open_wvlc(h);  break;
        case ID_COMPRESS:  action_compress(h);   break;
        case ID_SAVE_IMG:  action_save_image(h); break;
        case ID_LOSSLESS:
            if (HIWORD(wp) == BN_CLICKED) {
                EnableWindow(g_track, !is_lossless());
                update_quality_label();
            }
            break;
        case ID_SPLIT:
            if (HIWORD(wp) == BN_CLICKED) InvalidateRect(h, NULL, TRUE);
            break;
        }
        return 0;
    case WM_DESTROY:
        img_free(&g_source); img_free(&g_view);
        if (g_font) { DeleteObject(g_font); g_font = NULL; }
        if (g_brToolbar) { DeleteObject(g_brToolbar); g_brToolbar = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* ---------------- headless self-test ---------------- */

static FILE *g_log;
static void logf_(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    if (g_log) fputs(buf, g_log);
}

static int run_selftest(const char *input, const char *outdir) {
    char logpath[MAX_PATH];
    snprintf(logpath, sizeof logpath, "%s\\selftest.log", outdir);
    g_log = fopen(logpath, "w");

    Image src;
    if (!img_load(input, &src)) { logf_("selftest: cannot load %s\n", input); if (g_log) fclose(g_log); return 1; }
    logf_("loaded %dx%d %d-channel image\n", src.w, src.h, src.channels);
    int fail = 0;

    /* lossless roundtrip must be bit-exact */
    uint8_t *blob; size_t bl;
    if (wv_compress_image(src.px, src.w, src.h, src.channels, 0, WV_WAVELET_AUTO, -1, -1, &blob, &bl) != WV_OK)
        { printf("lossless compress failed\n"); return 1; }
    uint8_t *raw; size_t rl;
    wv_decompress(blob, bl, &raw, &rl);
    size_t n = (size_t)src.w * src.h * src.channels;
    int exact = (rl == n && memcmp(raw, src.px, n) == 0);
    logf_("lossless: %zu -> %zu bytes, exact=%s\n", n, bl, exact ? "yes" : "NO");
    if (!exact) fail = 1;
    wv_free(blob); wv_free(raw);

    /* lossy compress + reconstruct + render to a BMP */
    if (wv_compress_image(src.px, src.w, src.h, src.channels, 1, WV_WAVELET_AUTO, -1, 80, &blob, &bl) != WV_OK)
        { logf_("lossy compress failed\n"); if (g_log) fclose(g_log); return 1; }
    wv_decompress(blob, bl, &raw, &rl);
    Image recon = { src.w, src.h, src.channels, raw };
    double e = image_rmse(&src, &recon);
    logf_("lossy q80: %zu -> %zu bytes (%.1f%%), rmse=%.3f\n", n, bl, bl*100.0/n, e);

    /* verify the display-buffer path produces a valid image */
    uint8_t *bgr = img_to_bgr24(&recon);
    if (!bgr) { logf_("img_to_bgr24 failed\n"); fail = 1; }
    else free(bgr);

    char out[MAX_PATH];
    snprintf(out, sizeof out, "%s\\recon.bmp", outdir);
    if (img_save(out, &recon)) logf_("wrote reconstruction: %s\n", out);
    else { logf_("could not write %s\n", out); fail = 1; }

    wv_free(blob); free(raw); img_free(&src);
    logf_("%s\n", fail ? "SELFTEST FAILED" : "SELFTEST OK");
    if (g_log) fclose(g_log);
    return fail;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd;
    if (__argc >= 2 && strcmp(__argv[1], "--selftest") == 0) {
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        int rc = run_selftest(__argc > 2 ? __argv[2] : "", __argc > 3 ? __argv[3] : ".");
        return rc;
    }

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSA wc; ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;                 /* fully painted in WM_PAINT */
    wc.lpszClassName = "WaveletGuiWindow";
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);

    g_main = CreateWindowA("WaveletGuiWindow", "Wavelet Image Studio",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 960, 680,
                           NULL, NULL, inst, NULL);

    /* Size the initial window for the monitor's DPI (WM_CREATE already laid the
     * controls out at g_dpi; scale the outer frame to match). */
    if (g_dpi != 96) {
        RECT wr; GetWindowRect(g_main, &wr);
        SetWindowPos(g_main, NULL, 0, 0,
                     MulDiv(wr.right - wr.left, (int)g_dpi, 96),
                     MulDiv(wr.bottom - wr.top, (int)g_dpi, 96),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    if (__argc >= 2 && strcmp(__argv[1], "--selftest") != 0) {
        const char *path = __argv[1];
        size_t n = strlen(path);
        if (n >= 5 && _stricmp(path + n - 5, ".wvlc") == 0) {
            load_wvlc_path(g_main, path);
        } else {
            load_image_path(g_main, path);
        }
    }

    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) {
        if (IsDialogMessage(g_main, &m)) continue;   /* tab navigation */
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return 0;
}
