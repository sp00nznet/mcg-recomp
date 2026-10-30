/*
 * Putting the engine's frame on the host window.
 *
 * ddraw.c turns the engine's back buffer into a 32-bit picture; this file owns
 * that picture and everything after it: where it sits in the window (aspect
 * kept, bars around it), how it is scaled, the borderless-fullscreen window,
 * the inverse mapping for the mouse, and presenting it in step with the
 * display.
 *
 * Presenting runs on its own thread. The engine flips whenever it likes --
 * irregularly, and sometimes many times per refresh -- and blocking its thread
 * on the display would slow the whole game down, timer heartbeat included. So
 * the main thread only converts and hands over; the present thread blits the
 * newest finished frame and then waits for the compositor (DwmFlush), which is
 * vsync for a windowed GDI app. Three buffers, so neither side ever waits for
 * the other: the main thread fills WORK, swaps it with READY, and the present
 * thread swaps READY with SHOW when there is a new one. A frame that arrives
 * while another is still waiting simply replaces it.
 *
 * The present thread touches only host GDI objects, never simulated memory or
 * the lifted CPU state, which is what makes a second thread safe here at all
 * (see the timer note in shims_impl.c for why lifted code must stay on one).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>

#include "imports.h"

int g_fullscreen;            /* --fullscreen: borderless window over the monitor */
int g_scale_mode;            /* --scale: 0 sharp (nearest), 1 smooth, 2 integer */
int g_res_w, g_res_h;        /* --res WxH: the mode the engine is told to use */
int g_show_fps;              /* --fps: frame-time readout */

typedef struct {
    HDC dc;
    HBITMAP bmp;
    uint32_t* px;
    uint32_t w, h;           /* allocated size */
    uint32_t sw, sh;         /* the part of it that is the picture */
} frame_t;

static frame_t g_fr[3];
static int g_work = 0, g_ready = 1, g_show = 2;
static int g_fresh;
static CRITICAL_SECTION g_lock;
static HANDLE g_event, g_thread;
static HWND g_target;
static volatile LONG g_blits;    /* display-side frames, for --fps */

/* The rectangle inside a cw x ch client that an sw x sh picture fills, aspect
 * kept. Pillarboxed on a wide client, letterboxed on a tall one. */
void display_rect(int cw, int ch, int sw, int sh, RECT* r) {
    int dw = cw, dh = ch;
    if (sw <= 0 || sh <= 0 || cw <= 0 || ch <= 0) { SetRect(r, 0, 0, cw, ch); return; }
    if (g_scale_mode == 2 && cw >= sw && ch >= sh) {
        int k = cw / sw < ch / sh ? cw / sw : ch / sh;
        dw = sw * k;
        dh = sh * k;
    } else if ((long long)cw * sh > (long long)ch * sw) {
        dw = (int)((long long)ch * sw / sh);
    } else {
        dh = (int)((long long)cw * sh / sw);
    }
    r->left = (cw - dw) / 2;
    r->top = (ch - dh) / 2;
    r->right = r->left + dw;
    r->bottom = r->top + dh;
}

/* src (sw x sh) into dst (cw x ch): bars black, picture scaled. */
void display_draw(HDC dst, int cw, int ch, HDC src, int sw, int sh) {
    RECT r;
    display_rect(cw, ch, sw, sh, &r);
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (r.top > 0) { RECT b = { 0, 0, cw, r.top }; FillRect(dst, &b, black); }
    if (r.bottom < ch) { RECT b = { 0, r.bottom, cw, ch }; FillRect(dst, &b, black); }
    if (r.left > 0) { RECT b = { 0, r.top, r.left, r.bottom }; FillRect(dst, &b, black); }
    if (r.right < cw) { RECT b = { r.right, r.top, cw, r.bottom }; FillRect(dst, &b, black); }
    int dw = r.right - r.left, dh = r.bottom - r.top;
    if (dw == sw && dh == sh) {
        BitBlt(dst, r.left, r.top, sw, sh, src, 0, 0, SRCCOPY);
    } else {
        /* COLORONCOLOR is nearest-neighbour: crisp pixel art at any size.
         * HALFTONE is GDI's box filter -- soft, but even at odd ratios. */
        SetStretchBltMode(dst, g_scale_mode == 1 ? HALFTONE : COLORONCOLOR);
        SetBrushOrgEx(dst, 0, 0, NULL);
        StretchBlt(dst, r.left, r.top, dw, dh, src, 0, 0, sw, sh, SRCCOPY);
    }
}

/* A client-area point -> the picture's own pixels. Returns 0 outside it. */
int display_unmap(HWND h, int sw, int sh, POINT* p) {
    RECT rc, r;
    GetClientRect(h, &rc);
    display_rect(rc.right, rc.bottom, sw, sh, &r);
    if (r.right <= r.left || r.bottom <= r.top) return 0;
    if (p->x < r.left || p->y < r.top || p->x >= r.right || p->y >= r.bottom) return 0;
    p->x = (p->x - r.left) * sw / (r.right - r.left);
    p->y = (p->y - r.top) * sh / (r.bottom - r.top);
    return 1;
}

static DWORD WINAPI present_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        /* Wake for a new frame, or every 100 ms anyway so a window uncovered
         * or resized while the engine is idle still gets repainted. */
        WaitForSingleObject(g_event, 100);
        EnterCriticalSection(&g_lock);
        if (g_fresh) {
            int t = g_show; g_show = g_ready; g_ready = t;
            g_fresh = 0;
        }
        frame_t* f = &g_fr[g_show];
        LeaveCriticalSection(&g_lock);
        if (!f->dc || !g_target) continue;
        RECT rc;
        GetClientRect(g_target, &rc);
        HDC wdc = GetDC(g_target);
        display_draw(wdc, rc.right, rc.bottom, f->dc, (int)f->sw, (int)f->sh);
        ReleaseDC(g_target, wdc);
        InterlockedIncrement(&g_blits);
        /* Wait for the compositor to take it: one blit per refresh at most,
         * and always a whole frame -- DWM never shows a half-drawn window. */
        DwmFlush();
    }
}

/* The buffer the main thread converts into, sized w x h, and a memory DC
 * with it selected. */
uint32_t* display_work(uint32_t w, uint32_t h, HDC* dc) {
    frame_t* f = &g_fr[g_work];
    *dc = f->dc;
    if (f->px && f->w == w && f->h == h) return f->px;
    if (f->bmp) DeleteObject(f->bmp);
    if (!f->dc) f->dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;           /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    f->bmp = CreateDIBSection(f->dc, &bi, DIB_RGB_COLORS, (void**)&f->px, NULL, 0);
    *dc = f->dc;
    if (!f->bmp) { f->px = NULL; f->w = f->h = 0; return NULL; }
    SelectObject(f->dc, f->bmp);
    f->w = w;
    f->h = h;
    return f->px;
}

/* --fps: frames the engine presented and frames that reached the display in
 * the last second, plus the worst gap between engine frames, drawn into the
 * corner of the picture (so it shows in a --record too). */
static void draw_fps(frame_t* f) {
    static DWORD t0, last, worst;
    static unsigned n;
    static char text[96];
    static LONG blits0;
    DWORD now = GetTickCount();
    if (last && now - last > worst) worst = now - last;
    last = now;
    n++;
    if (!t0) t0 = now;
    if (now - t0 >= 1000) {
        LONG b = g_blits;
        snprintf(text, sizeof text, " engine %u fps  display %ld fps  worst %lu ms ",
                 n * 1000 / (now - t0), (long)(b - blits0) * 1000 / (long)(now - t0),
                 (unsigned long)worst);
        fprintf(stderr, "[fps]%s\n", text);
        blits0 = b;
        n = 0;
        worst = 0;
        t0 = now;
    }
    if (text[0]) {
        SetBkColor(f->dc, RGB(0, 0, 0));
        SetTextColor(f->dc, RGB(255, 255, 0));
        TextOutA(f->dc, 4, 4, text, (int)strlen(text));
    }
}

/* The work buffer holds a finished frame whose top-left sw x sh is the picture:
 * hand it to the present thread. */
void display_submit(HWND h, uint32_t sw, uint32_t sh) {
    if (!g_thread) {
        InitializeCriticalSection(&g_lock);
        g_event = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_thread = CreateThread(NULL, 0, present_thread, NULL, 0, NULL);
    }
    g_target = h;
    frame_t* f = &g_fr[g_work];
    f->sw = sw;
    f->sh = sh;
    if (g_show_fps) draw_fps(f);
    GdiFlush();
    EnterCriticalSection(&g_lock);
    int t = g_ready; g_ready = g_work; g_work = t;
    g_fresh = 1;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_event);
}
