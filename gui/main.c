/*
 * main.c -- Win32 GUI for the wavelet image compressor.
 *
 * Load a source image (BMP / PGM / PPM), compress it to a .wvlc container with
 * the wavelet library, preview the reconstruction with size + RMSE stats, open
 * and view existing .wvlc images, and export the viewed image back to an image
 * file. Links against wavelet.dll.
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

#define TOOLBAR_H 46
#define STATUS_H  26

static HWND g_main, g_track, g_qval, g_chkLossless, g_chkSplit, g_btnCompress, g_btnSave, g_status;
static Image  g_source;       /* original / source image (for compression + RMSE) */
static Image  g_view;         /* currently displayed reconstruction / image        */
static int    g_quality = 80;
static int    g_have_recon = 0;   /* g_view is a reconstruction distinct from source */
static double g_last_rmse = -1.0;

/* ---------------- utilities ---------------- */

static void set_status(const char *fmt, ...) {
    char buf[600];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_status) SetWindowTextA(g_status, buf);
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
    if (is_lossless()) strcpy(b, "lossless");
    else sprintf(b, "Q %d", g_quality);
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
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameA(&ofn);
}
static int dlg_save(HWND h, const char *filter, const char *defext, char *out, size_t n) {
    OPENFILENAMEA ofn; ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = h;
    ofn.lpstrFilter = filter; ofn.lpstrFile = out; ofn.nMaxFile = (DWORD)n;
    ofn.lpstrDefExt = defext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
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
    RECT bar = { pane.left, pane.top, pane.right, pane.top + 22 };
    HBRUSH b = CreateSolidBrush(RGB(20, 22, 26));
    FillRect(hdc, &bar, b);
    DeleteObject(b);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(210, 214, 220));
    bar.left += 8;
    DrawTextA(hdc, text, -1, &bar, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    RECT area = { 0, TOOLBAR_H, rc.right, rc.bottom - STATUS_H };

    HBRUSH bg = CreateSolidBrush(RGB(32, 34, 38));
    FillRect(hdc, &area, bg);
    DeleteObject(bg);

    int split = (SendMessageA(g_chkSplit, BM_GETCHECK, 0, 0) == BST_CHECKED)
                && g_source.px && g_view.px;

    if (split) {
        int mid = (area.left + area.right) / 2;
        RECT left  = { area.left, area.top, mid - 1, area.bottom };
        RECT right = { mid + 1, area.top, area.right, area.bottom };
        RECT limg = { left.left, left.top + 22, left.right, left.bottom };
        RECT rimg = { right.left, right.top + 22, right.right, right.bottom };
        draw_image_in(hdc, &g_source, limg);
        draw_image_in(hdc, &g_view, rimg);
        /* divider */
        RECT div = { mid - 1, area.top, mid + 1, area.bottom };
        HBRUSH d = CreateSolidBrush(RGB(20, 22, 26)); FillRect(hdc, &div, d); DeleteObject(d);
        char cap[128];
        draw_caption(hdc, left, "Original");
        if (g_have_recon && g_last_rmse >= 0) sprintf(cap, "Reconstruction   RMSE %.2f", g_last_rmse);
        else strcpy(cap, "Reconstruction");
        draw_caption(hdc, right, cap);
    } else if (g_view.px) {
        draw_image_in(hdc, &g_view, area);
    } else {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(150, 150, 155));
        const char *msg = "Open an image to compress, or open a .wvlc to view.  (drag & drop supported)";
        DrawTextA(hdc, msg, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    EndPaint(h, &ps);
}

/* ---------------- window ---------------- */

static HWND mk_button(HWND parent, const char *text, int id, int x, int w) {
    return CreateWindowA("BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                         x, 9, w, 28, parent, (HMENU)(INT_PTR)id, NULL, NULL);
}

static void create_controls(HWND h) {
    int x = 8;
    mk_button(h, "Open Image\x85", ID_OPEN_IMG, x, 108); x += 114;
    mk_button(h, "Open .wvlc\x85", ID_OPEN_WVLC, x, 104); x += 110;
    g_btnCompress = mk_button(h, "Compress && Save\x85", ID_COMPRESS, x, 138); x += 144;
    g_btnSave = mk_button(h, "Save Image\x85", ID_SAVE_IMG, x, 104); x += 116;

    g_chkLossless = CreateWindowA("BUTTON", "Lossless", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                  x, 13, 84, 20, h, (HMENU)ID_LOSSLESS, NULL, NULL);
    x += 90;

    g_track = CreateWindowA(TRACKBAR_CLASS, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                            x, 11, 150, 26, h, (HMENU)ID_QUALITY, NULL, NULL);
    SendMessageA(g_track, TBM_SETRANGE, TRUE, MAKELONG(1, 100));
    SendMessageA(g_track, TBM_SETPOS, TRUE, g_quality);
    x += 156;

    g_qval = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT,
                           x, 13, 72, 20, h, NULL, NULL, NULL);
    x += 76;

    g_chkSplit = CreateWindowA("BUTTON", "Split view", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               x, 13, 96, 20, h, (HMENU)ID_SPLIT, NULL, NULL);

    g_status = CreateWindowA("STATIC", "Ready.", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
                             8, 0, 100, STATUS_H, h, NULL, NULL, NULL);

    EnableWindow(g_btnCompress, FALSE);
    EnableWindow(g_btnSave, FALSE);
    update_quality_label();
}

static void layout(HWND h) {
    RECT rc; GetClientRect(h, &rc);
    MoveWindow(g_status, 8, rc.bottom - STATUS_H, rc.right - 16, STATUS_H, TRUE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:      create_controls(h); DragAcceptFiles(h, TRUE); return 0;
    case WM_SIZE:        layout(h); return 0;
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
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "WaveletGuiWindow";
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_APPICON));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);

    g_main = CreateWindowA("WaveletGuiWindow", "Wavelet Image Studio",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 960, 680,
                           NULL, NULL, inst, NULL);
    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) {
        if (IsDialogMessage(g_main, &m)) continue;   /* tab navigation */
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return 0;
}
