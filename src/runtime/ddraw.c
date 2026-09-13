/*
 * A DirectDraw the engine can actually use.
 *
 * MCX.EXE imports exactly one symbol from DDRAW -- DirectDrawCreate -- and
 * reaches everything else through COM vtables. So there is no import to shim:
 * what is needed is an object graph living in SIMULATED memory whose vtable
 * slots are addresses the dispatcher can resolve back to C functions here.
 *
 * That is the same trick the imagehlp exports use, with its own address range:
 * each method gets a synthetic VA, the vtable is an array of those VAs built in
 * the arena, and recomp_lookup_import maps them back. A lifted
 * `call dword ptr [eax + 0x54]` then lands in ddraw_SetDisplayMode below.
 *
 * The model is deliberately small. MechCommander is a software renderer: it
 * locks a surface, writes 8-bit pixels into it, and flips. So the back buffer
 * is arena memory the engine draws into directly, and presenting is a palette
 * lookup into a 32-bit DIB that the host BitBlts to the window. No hardware
 * blitting is emulated beyond what the engine actually asks for.
 *
 * ponytail: colour-fill and surface-to-surface Blt only, which is what the
 * engine uses. Anything else says so and returns DDERR_UNSUPPORTED rather than
 * quietly succeeding.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"
#include "imports.h"

#define DD_OK                 0x00000000u
#define DDERR_UNSUPPORTED     0x80004001u
#define DDERR_INVALIDPARAMS   0x80070057u
#define E_NOINTERFACE         0x80004002u

/* ---- synthetic COM method addresses -------------------------------------
 * Above the runtime-export range (0x11000000) and far above every simulated
 * region, so one of these used as a pointer faults rather than landing in
 * real memory. */
#define COM_VA_BASE 0x12000000u
#define COM_MAX     256

static recomp_func_t g_com[COM_MAX];
static uint32_t g_com_n = 1;              /* slot 0 stays null */

recomp_func_t ddraw_lookup(uint32_t va) {
    if (va < COM_VA_BASE || (va & 3u))
        return NULL;
    uint32_t i = (va - COM_VA_BASE) / 4;
    return i < g_com_n ? g_com[i] : NULL;
}

static uint32_t com_slot(recomp_func_t f) {
    if (!f || g_com_n >= COM_MAX) return 0;
    g_com[g_com_n] = f;
    return COM_VA_BASE + (g_com_n++) * 4;
}

/* ---- object model -------------------------------------------------------
 * Every object is arena memory whose first dword is its vtable VA; the fields
 * after that are ours, and the engine never looks at them.
 */
#define OBJ_VTABLE 0
#define OBJ_KIND   4
#define OBJ_INDEX  8
#define OBJ_SIZE   16

#define KIND_DD      1
#define KIND_SURFACE 2
#define KIND_PALETTE 3

#define MAX_SURFACES 64

typedef struct {
    uint32_t obj;          /* the simulated object */
    uint32_t bits;         /* arena pixels, 8bpp */
    uint32_t width, height, pitch;
    uint32_t caps;
    int is_primary;
    int back;              /* index of the attached back buffer, -1 if none */
    uint32_t palette;      /* simulated palette object */
} surface_t;

static surface_t g_surf[MAX_SURFACES];
static uint32_t g_surf_n;

static RGBQUAD g_pal[256];
static HWND g_hwnd;
static uint32_t g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 8;

/* One 32-bit DIB, reused: the palette lookup target the host can blit. */
static HDC g_memdc;
static HBITMAP g_dib;
static uint32_t* g_dib_px;
static uint32_t g_dib_w, g_dib_h;

static surface_t* surf_of(uint32_t obj) {
    if (!obj || MEM32(obj + OBJ_KIND) != KIND_SURFACE) return NULL;
    uint32_t i = MEM32(obj + OBJ_INDEX);
    return i < g_surf_n ? &g_surf[i] : NULL;
}

/* ---- presenting ---------------------------------------------------------
 * 8-bit indexed through the current palette into a 32-bit DIB, then one
 * BitBlt. A 640x480 frame is 300K pixels; at 30 Hz that is nothing next to
 * everything else the engine is doing, and it keeps the path obvious.
 */
static void present(surface_t* s) {
    if (!s || !s->bits || !g_hwnd) return;

    if (!g_dib || g_dib_w != s->width || g_dib_h != s->height) {
        if (g_dib) { DeleteObject(g_dib); g_dib = NULL; }
        if (!g_memdc) {
            HDC wdc = GetDC(g_hwnd);
            g_memdc = CreateCompatibleDC(wdc);
            ReleaseDC(g_hwnd, wdc);
        }
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = (LONG)s->width;
        bi.bmiHeader.biHeight = -(LONG)s->height;   /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g_dib = CreateDIBSection(g_memdc, &bi, DIB_RGB_COLORS,
                                 (void**)&g_dib_px, NULL, 0);
        if (!g_dib) return;
        g_dib_w = s->width;
        g_dib_h = s->height;
        fprintf(stderr, "[ddraw] present target %ux%u\n", g_dib_w, g_dib_h);
    }

    const uint8_t* src = (const uint8_t*)ADDR(s->bits);
    for (uint32_t y = 0; y < s->height; y++) {
        const uint8_t* row = src + (size_t)y * s->pitch;
        uint32_t* out = g_dib_px + (size_t)y * s->width;
        for (uint32_t x = 0; x < s->width; x++) {
            RGBQUAD c = g_pal[row[x]];
            out[x] = ((uint32_t)c.rgbRed << 16) | ((uint32_t)c.rgbGreen << 8)
                     | c.rgbBlue;
        }
    }

    HDC wdc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(g_memdc, g_dib);
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    if ((uint32_t)rc.right == s->width && (uint32_t)rc.bottom == s->height) {
        BitBlt(wdc, 0, 0, (int)s->width, (int)s->height, g_memdc, 0, 0, SRCCOPY);
    } else {
        SetStretchBltMode(wdc, COLORONCOLOR);
        StretchBlt(wdc, 0, 0, rc.right, rc.bottom, g_memdc, 0, 0,
                   (int)s->width, (int)s->height, SRCCOPY);
    }
    SelectObject(g_memdc, old);
    ReleaseDC(g_hwnd, wdc);
}

/* ---- helpers ------------------------------------------------------------ */

/* DDSURFACEDESC is 108 bytes; only the fields the engine reads are filled. */
#define SD_SIZE     0x00
#define SD_FLAGS    0x04
#define SD_HEIGHT   0x08
#define SD_WIDTH    0x0C
#define SD_PITCH    0x10
#define SD_BACKCNT  0x14
#define SD_SURFACE  0x24
#define SD_PIXFMT   0x48
#define SD_CAPS     0x68

#define DDSD_CAPS 0x1u
#define DDSD_HEIGHT 0x2u
#define DDSD_WIDTH 0x4u
#define DDSD_PITCH 0x8u
#define DDSD_BACKBUFFERCOUNT 0x20u
#define DDSD_PIXELFORMAT 0x1000u
#define DDSD_LPSURFACE 0x800u

#define DDSCAPS_PRIMARYSURFACE 0x200u
#define DDSCAPS_BACKBUFFER     0x4u
#define DDSCAPS_FLIP           0x10u
#define DDSCAPS_OFFSCREENPLAIN 0x40u
#define DDSCAPS_SYSTEMMEMORY   0x800u
#define DDSCAPS_VIDEOMEMORY    0x4000u

static uint32_t new_object(uint32_t vtable, uint32_t kind, uint32_t index) {
    uint32_t o = shims_alloc(OBJ_SIZE);
    if (!o) return 0;
    MEM32(o + OBJ_VTABLE) = vtable;
    MEM32(o + OBJ_KIND) = kind;
    MEM32(o + OBJ_INDEX) = index;
    return o;
}

static uint32_t g_vt_surface;      /* built once, shared by every surface */
static uint32_t g_vt_palette;

static uint32_t make_surface(uint32_t w, uint32_t h, uint32_t caps) {
    if (g_surf_n >= MAX_SURFACES || !w || !h) return 0;
    surface_t* s = &g_surf[g_surf_n];
    memset(s, 0, sizeof *s);
    s->width = w;
    s->height = h;
    s->pitch = (w + 3u) & ~3u;             /* DWORD-aligned rows, like the real one */
    s->bits = shims_alloc(s->pitch * h);
    s->caps = caps;
    s->back = -1;
    s->is_primary = (caps & DDSCAPS_PRIMARYSURFACE) != 0;
    s->obj = new_object(g_vt_surface, KIND_SURFACE, g_surf_n);
    g_surf_n++;
    return s->obj;
}

/* ---- IDirectDrawSurface ------------------------------------------------- */

/* IDirectDrawSurface2/3/4 differ only in methods this engine does not call, so
 * a surface answers with itself rather than growing three near-identical
 * tables. If one of those later methods is ever reached it lands on a null
 * vtable slot, which the dispatcher reports by address. */
static void sf_QueryInterface(void) {
    if (!ARG(2)) { RET(DDERR_INVALIDPARAMS); STDRET(3); return; }
    MEM32(ARG(2)) = ARG(0);
    RET(DD_OK);
    STDRET(3);
}
static void sf_AddRef(void)  { RET(1); STDRET(1); }
static void sf_Release(void) { RET(0); STDRET(1); }
static void sf_IsLost(void)  { RET(DD_OK); STDRET(1); }
static void sf_Restore(void) { RET(DD_OK); STDRET(1); }

/* Shared by GetSurfaceDesc and Lock, so neither has to borrow the other's
 * epilogue -- each shim owns exactly one STDRET. */
static void fill_desc(surface_t* s, uint32_t d) {
    MEM32(d + SD_FLAGS) = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH
                          | DDSD_PIXELFORMAT;
    MEM32(d + SD_HEIGHT) = s->height;
    MEM32(d + SD_WIDTH) = s->width;
    MEM32(d + SD_PITCH) = s->pitch;
    MEM32(d + SD_CAPS) = s->caps;
    MEM32(d + SD_PIXFMT + 0x00) = 32;          /* dwSize */
    MEM32(d + SD_PIXFMT + 0x04) = 0x20;        /* DDPF_RGB */
    MEM32(d + SD_PIXFMT + 0x0C) = 8;           /* dwRGBBitCount */
}

static void sf_GetSurfaceDesc(void) {
    surface_t* s = surf_of(ARG(0));
    uint32_t d = ARG(1);
    if (!s || !d) { RET(DDERR_INVALIDPARAMS); STDRET(2); return; }
    fill_desc(s, d);
    RET(DD_OK);
    STDRET(2);
}

static void sf_Lock(void) {
    surface_t* s = surf_of(ARG(0));
    uint32_t d = ARG(2);
    if (!s || !d) { RET(DDERR_INVALIDPARAMS); STDRET(5); return; }
    fill_desc(s, d);
    MEM32(d + SD_FLAGS) = MEM32(d + SD_FLAGS) | DDSD_LPSURFACE;
    MEM32(d + SD_SURFACE) = s->bits;           /* the engine draws straight here */
    RET(DD_OK);
    STDRET(5);
}

static void sf_Unlock(void) {
    surface_t* s = surf_of(ARG(0));
    /* The primary is what the window shows, so an unlock of it is a present.
     * A back buffer waits for its Flip. */
    if (s && s->is_primary && s->back < 0) present(s);
    RET(DD_OK);
    STDRET(2);
}

static void sf_Flip(void) {
    surface_t* s = surf_of(ARG(0));
    if (s) present(s->back >= 0 ? &g_surf[s->back] : s);
    RET(DD_OK);
    STDRET(3);
}

static void sf_GetAttachedSurface(void) {
    surface_t* s = surf_of(ARG(0));
    if (!s || s->back < 0 || !ARG(2)) { RET(DDERR_INVALIDPARAMS); STDRET(3); return; }
    MEM32(ARG(2)) = g_surf[s->back].obj;
    RET(DD_OK);
    STDRET(3);
}

static void sf_GetCaps(void) {
    surface_t* s = surf_of(ARG(0));
    if (ARG(1)) MEM32(ARG(1)) = s ? s->caps : 0;
    RET(DD_OK);
    STDRET(2);
}

static void sf_SetPalette(void) {
    surface_t* s = surf_of(ARG(0));
    if (s) s->palette = ARG(1);
    RET(DD_OK);
    STDRET(2);
}

static void sf_GetPalette(void) {
    surface_t* s = surf_of(ARG(0));
    if (!s || !s->palette || !ARG(1)) { RET(DDERR_INVALIDPARAMS); STDRET(2); return; }
    MEM32(ARG(1)) = s->palette;
    RET(DD_OK);
    STDRET(2);
}

static void sf_SetClipper(void)   { RET(DD_OK); STDRET(2); }
static void sf_SetColorKey(void)  { RET(DD_OK); STDRET(3); }
static void sf_GetPixelFormat(void) {
    uint32_t p = ARG(1);
    if (p) { MEM32(p) = 32; MEM32(p + 4) = 0x20; MEM32(p + 0x0C) = 8; }
    RET(DD_OK);
    STDRET(2);
}

/* Blt: colour fill and straight surface-to-surface copy, which is all the
 * engine asks for. Stretching and effects report unsupported rather than
 * silently doing the wrong thing. */
static void sf_Blt(void) {
    surface_t* d = surf_of(ARG(0));
    uint32_t drect = ARG(1);
    surface_t* s = surf_of(ARG(2));
    uint32_t srect = ARG(3);
    uint32_t flags = ARG(4), fx = ARG(5);

    int dx = 0, dy = 0, w, h;
    if (!d) { RET(DDERR_INVALIDPARAMS); STDRET(6); return; }
    w = (int)d->width; h = (int)d->height;
    if (drect) {
        dx = (int)MEM32(drect); dy = (int)MEM32(drect + 4);
        w = (int)MEM32(drect + 8) - dx;
        h = (int)MEM32(drect + 12) - dy;
    }
    if (dx < 0 || dy < 0 || w <= 0 || h <= 0) { RET(DD_OK); STDRET(6); return; }
    if (dx + w > (int)d->width) w = (int)d->width - dx;
    if (dy + h > (int)d->height) h = (int)d->height - dy;

    if ((flags & 0x400u) && fx) {              /* DDBLT_COLORFILL */
        uint8_t c = (uint8_t)MEM32(fx + 0x54); /* DDBLTFX.dwFillColor */
        uint8_t* dst = (uint8_t*)ADDR(d->bits);
        for (int y = 0; y < h; y++)
            memset(dst + (size_t)(dy + y) * d->pitch + dx, c, (size_t)w);
    } else if (s) {
        int sx = 0, sy = 0;
        if (srect) { sx = (int)MEM32(srect); sy = (int)MEM32(srect + 4); }
        const uint8_t* sp = (const uint8_t*)ADDR(s->bits);
        uint8_t* dp = (uint8_t*)ADDR(d->bits);
        for (int y = 0; y < h; y++) {
            if ((uint32_t)(sy + y) >= s->height) break;
            memcpy(dp + (size_t)(dy + y) * d->pitch + dx,
                   sp + (size_t)(sy + y) * s->pitch + sx, (size_t)w);
        }
    }
    if (d->is_primary) present(d);
    RET(DD_OK);
    STDRET(6);
}

static void sf_BltFast(void) {
    surface_t* d = surf_of(ARG(0));
    surface_t* s = surf_of(ARG(3));
    uint32_t srect = ARG(4);
    if (!d || !s) { RET(DDERR_INVALIDPARAMS); STDRET(6); return; }
    int dx = (int)ARG(1), dy = (int)ARG(2);
    int sx = 0, sy = 0, w = (int)s->width, h = (int)s->height;
    if (srect) {
        sx = (int)MEM32(srect); sy = (int)MEM32(srect + 4);
        w = (int)MEM32(srect + 8) - sx;
        h = (int)MEM32(srect + 12) - sy;
    }
    const uint8_t* sp = (const uint8_t*)ADDR(s->bits);
    uint8_t* dp = (uint8_t*)ADDR(d->bits);
    for (int y = 0; y < h; y++) {
        if ((uint32_t)(dy + y) >= d->height || (uint32_t)(sy + y) >= s->height) break;
        int n = w;
        if (dx + n > (int)d->width) n = (int)d->width - dx;
        if (n <= 0) break;
        memcpy(dp + (size_t)(dy + y) * d->pitch + dx,
               sp + (size_t)(sy + y) * s->pitch + sx, (size_t)n);
    }
    if (d->is_primary) present(d);
    RET(DD_OK);
    STDRET(6);
}

static void sf_GetDC(void) {
    /* The engine only takes a DC to draw text over a locked surface. Give it
     * the shared memory DC; nothing here maps GDI output back into the 8-bit
     * buffer, so this is honest about doing nothing. */
    if (ARG(1)) MEM32(ARG(1)) = 0;
    RET(DDERR_UNSUPPORTED);
    STDRET(2);
}
static void sf_ReleaseDC(void) { RET(DD_OK); STDRET(2); }

/* ---- IDirectDrawPalette ------------------------------------------------- */

static void pal_QueryInterface(void) { RET(E_NOINTERFACE); STDRET(3); }
static void pal_AddRef(void)  { RET(1); STDRET(1); }
static void pal_Release(void) { RET(0); STDRET(1); }
static void pal_GetCaps(void) { if (ARG(1)) MEM32(ARG(1)) = 0; RET(DD_OK); STDRET(2); }
static void pal_Initialize(void) { RET(DD_OK); STDRET(4); }

static void pal_read_entries(uint32_t start, uint32_t count, uint32_t src) {
    if (!src) return;
    for (uint32_t i = 0; i < count && start + i < 256; i++) {
        uint32_t e = MEM32(src + i * 4);        /* PALETTEENTRY: r,g,b,flags */
        g_pal[start + i].rgbRed   = (BYTE)(e & 0xFF);
        g_pal[start + i].rgbGreen = (BYTE)((e >> 8) & 0xFF);
        g_pal[start + i].rgbBlue  = (BYTE)((e >> 16) & 0xFF);
    }
}

static void pal_SetEntries(void) {
    pal_read_entries(ARG(2), ARG(3), ARG(4));
    /* A palette change repaints what is already on screen. */
    for (uint32_t i = 0; i < g_surf_n; i++)
        if (g_surf[i].is_primary) { present(g_surf[i].back >= 0 ? &g_surf[g_surf[i].back] : &g_surf[i]); break; }
    RET(DD_OK);
    STDRET(5);
}

static void pal_GetEntries(void) {
    uint32_t start = ARG(2), count = ARG(3), dst = ARG(4);
    for (uint32_t i = 0; dst && i < count && start + i < 256; i++) {
        RGBQUAD c = g_pal[start + i];
        MEM32(dst + i * 4) = (uint32_t)c.rgbRed | ((uint32_t)c.rgbGreen << 8)
                             | ((uint32_t)c.rgbBlue << 16);
    }
    RET(DD_OK);
    STDRET(5);
}

/* ---- IDirectDraw -------------------------------------------------------- */

/*
 * IDirectDraw2, which is what a DirectX 6 title actually uses: DirectDrawCreate
 * hands back the original interface and the engine immediately queries for the
 * newer one. It is the same vtable through SetCooperativeLevel, then
 * SetDisplayMode grows two arguments (refresh rate and flags) and
 * GetAvailableVidMem is appended -- so it needs its own table, not a cast.
 */
static void dd2_SetDisplayMode(void) {
    g_mode_w = ARG(1);
    g_mode_h = ARG(2);
    g_mode_bpp = ARG(3);
    fprintf(stderr, "[ddraw] SetDisplayMode2 %ux%u %ubpp\n",
            g_mode_w, g_mode_h, g_mode_bpp);
    if (g_hwnd) {
        RECT r = { 0, 0, (LONG)g_mode_w, (LONG)g_mode_h };
        AdjustWindowRect(&r, (DWORD)GetWindowLongA(g_hwnd, GWL_STYLE), FALSE);
        SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER);
        ShowWindow(g_hwnd, SW_SHOW);
    }
    RET(g_mode_bpp == 8 ? DD_OK : DDERR_UNSUPPORTED);
    STDRET(6);
}

/* Plenty of room, so nothing declines an allocation for being short of it. */
static void dd_GetAvailableVidMem(void) {
    if (ARG(2)) MEM32(ARG(2)) = 32u << 20;
    if (ARG(3)) MEM32(ARG(3)) = 32u << 20;
    RET(DD_OK);
    STDRET(4);
}

static uint32_t g_vt_dd2;

static void dd_QueryInterface(void) {
    uint32_t riid = ARG(1), out = ARG(2);
    if (!out) { RET(DDERR_INVALIDPARAMS); STDRET(3); return; }
    /* Every IDirectDraw* IID is accepted and answered with the IDirectDraw2
     * table. The engine only ever asks for a newer DirectDraw here, and the
     * first dword of the GUID is logged so an unexpected one is visible. */
    fprintf(stderr, "[ddraw] QueryInterface {%08X...} -> IDirectDraw2\n",
            riid ? MEM32(riid) : 0);
    MEM32(out) = new_object(g_vt_dd2, KIND_DD, 0);
    RET(DD_OK);
    STDRET(3);
}
static void dd_AddRef(void)  { RET(1); STDRET(1); }
static void dd_Release(void) { RET(0); STDRET(1); }
static void dd_Compact(void) { RET(DD_OK); STDRET(1); }
static void dd_FlipToGDISurface(void) { RET(DD_OK); STDRET(1); }
static void dd_RestoreDisplayMode(void) { RET(DD_OK); STDRET(1); }
static void dd_WaitForVerticalBlank(void) { RET(DD_OK); STDRET(3); }
static void dd_CreateClipper(void) { if (ARG(2)) MEM32(ARG(2)) = 0; RET(DDERR_UNSUPPORTED); STDRET(4); }

static void dd_SetCooperativeLevel(void) {
    g_hwnd = (HWND)shims_handle(ARG(1));
    fprintf(stderr, "[ddraw] SetCooperativeLevel(hwnd=%p flags=%08X)\n",
            (void*)g_hwnd, ARG(2));
    RET(DD_OK);
    STDRET(3);
}

static void dd_SetDisplayMode(void) {
    g_mode_w = ARG(1);
    g_mode_h = ARG(2);
    g_mode_bpp = ARG(3);
    fprintf(stderr, "[ddraw] SetDisplayMode %ux%u %ubpp\n",
            g_mode_w, g_mode_h, g_mode_bpp);
    /* Run windowed at the requested size: a real mode switch is neither
     * possible nor wanted here, and the window is ours to resize. */
    if (g_hwnd) {
        RECT r = { 0, 0, (LONG)g_mode_w, (LONG)g_mode_h };
        AdjustWindowRect(&r, (DWORD)GetWindowLongA(g_hwnd, GWL_STYLE), FALSE);
        SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER);
        ShowWindow(g_hwnd, SW_SHOW);
    }
    RET(g_mode_bpp == 8 ? DD_OK : DDERR_UNSUPPORTED);
    STDRET(4);
}

static void dd_GetDisplayMode(void) {
    uint32_t d = ARG(1);
    if (!d) { RET(DDERR_INVALIDPARAMS); STDRET(2); return; }
    MEM32(d + SD_FLAGS) = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    MEM32(d + SD_HEIGHT) = g_mode_h;
    MEM32(d + SD_WIDTH) = g_mode_w;
    MEM32(d + SD_PITCH) = (g_mode_w + 3u) & ~3u;
    MEM32(d + SD_PIXFMT + 0x00) = 32;
    MEM32(d + SD_PIXFMT + 0x04) = 0x20;
    MEM32(d + SD_PIXFMT + 0x0C) = g_mode_bpp;
    RET(DD_OK);
    STDRET(2);
}

/* DDCAPS is large and the engine only tests capability bits. Zeroing it and
 * setting dwSize says "a device with no acceleration", which is true. */
static void dd_GetCaps(void) {
    for (int which = 1; which <= 2; which++) {
        uint32_t p = ARG((uint32_t)which);
        if (!p) continue;
        uint32_t size = MEM32(p);
        if (size < 4 || size > 512) size = 380;
        memset((void*)ADDR(p), 0, size);
        MEM32(p) = size;
        MEM32(p + 4) = 0x00004000u;       /* DDCAPS_BLTCOLORFILL */
    }
    RET(DD_OK);
    STDRET(3);
}

static void dd_CreateSurface(void) {
    uint32_t d = ARG(1), out = ARG(2);
    if (!d || !out) { RET(DDERR_INVALIDPARAMS); STDRET(4); return; }
    uint32_t flags = MEM32(d + SD_FLAGS);
    uint32_t caps = MEM32(d + SD_CAPS);
    uint32_t w = (flags & DDSD_WIDTH) ? MEM32(d + SD_WIDTH) : g_mode_w;
    uint32_t h = (flags & DDSD_HEIGHT) ? MEM32(d + SD_HEIGHT) : g_mode_h;
    uint32_t backs = (flags & DDSD_BACKBUFFERCOUNT) ? MEM32(d + SD_BACKCNT) : 0;

    if (caps & DDSCAPS_PRIMARYSURFACE) { w = g_mode_w; h = g_mode_h; }
    uint32_t obj = make_surface(w, h, caps);
    if (!obj) { RET(DDERR_INVALIDPARAMS); STDRET(4); return; }
    surface_t* s = surf_of(obj);

    if (backs) {                          /* a flipping chain: attach one back */
        uint32_t b = make_surface(w, h, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP);
        s = surf_of(obj);                 /* make_surface may have grown the array */
        if (b) s->back = (int)MEM32(b + OBJ_INDEX);
    }
    fprintf(stderr, "[ddraw] CreateSurface %ux%u caps=%08X back=%d -> bits 0x%08X\n",
            w, h, caps, s->back, s->bits);
    MEM32(out) = obj;
    RET(DD_OK);
    STDRET(4);
}

static void dd_CreatePalette(void) {
    uint32_t entries = ARG(2), out = ARG(3);
    if (!out) { RET(DDERR_INVALIDPARAMS); STDRET(5); return; }
    pal_read_entries(0, 256, entries);
    MEM32(out) = new_object(g_vt_palette, KIND_PALETTE, 0);
    RET(DD_OK);
    STDRET(5);
}

/* ============================================================
 * DirectSound
 *
 * Same shape as above and for the same reason: DSOUND is imported by ordinal
 * (DirectSoundCreate) and everything after it is vtable dispatch, so a stub
 * returning failure leaves SoundRendererInstall dereferencing a null
 * interface. Buffers are real arena memory the engine can lock and fill; that
 * memory is simply never played.
 *
 * ponytail: silent. Buffers accept audio and report position 0, which is
 * enough for the engine to run its mixer and keep time. Wiring the same
 * buffers to a host audio API is the obvious next step and changes nothing
 * above this line.
 * ============================================================ */

#define KIND_DSOUND  4
#define KIND_DSBUF   5

#define MAX_DSBUFS 64
typedef struct { uint32_t bytes, mem; } dsbuf_t;
static dsbuf_t g_dsbuf[MAX_DSBUFS];
static uint32_t g_dsbuf_n;
static uint32_t g_vt_dsbuf;

static dsbuf_t* dsbuf_of(uint32_t obj) {
    if (!obj || MEM32(obj + OBJ_KIND) != KIND_DSBUF) return NULL;
    uint32_t i = MEM32(obj + OBJ_INDEX);
    return i < g_dsbuf_n ? &g_dsbuf[i] : NULL;
}

static void ds_QueryInterface(void) {
    if (ARG(2)) MEM32(ARG(2)) = ARG(0);
    RET(DD_OK);
    STDRET(3);
}
static void ds_AddRef(void)  { RET(1); STDRET(1); }
static void ds_Release(void) { RET(0); STDRET(1); }
static void ds_Compact(void) { RET(DD_OK); STDRET(1); }
static void ds_SetCooperativeLevel(void) { RET(DD_OK); STDRET(3); }
static void ds_Initialize(void) { RET(DD_OK); STDRET(2); }
static void ds_GetSpeakerConfig(void) { if (ARG(1)) MEM32(ARG(1)) = 5; RET(DD_OK); STDRET(2); }
static void ds_SetSpeakerConfig(void) { RET(DD_OK); STDRET(2); }

static void ds_GetCaps(void) {
    uint32_t p = ARG(1);
    if (p) {
        uint32_t size = MEM32(p);
        if (size < 4 || size > 256) size = 96;
        memset((void*)ADDR(p), 0, size);
        MEM32(p) = size;
        MEM32(p + 4) = 0x200u;            /* DSCAPS_PRIMARY16BIT */
        MEM32(p + 12) = 32;               /* dwMaxHwMixingAllBuffers */
    }
    RET(DD_OK);
    STDRET(2);
}

/* DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, dwReserved, lpwfxFormat */
static void ds_CreateSoundBuffer(void) {
    uint32_t desc = ARG(1), out = ARG(2);
    if (!desc || !out || g_dsbuf_n >= MAX_DSBUFS) {
        RET(DDERR_INVALIDPARAMS);
        STDRET(4);
        return;
    }
    uint32_t bytes = MEM32(desc + 8);
    if (!bytes) bytes = 4096;             /* a primary buffer declares none */
    dsbuf_t* b = &g_dsbuf[g_dsbuf_n];
    b->bytes = bytes;
    b->mem = shims_alloc(bytes);
    MEM32(out) = new_object(g_vt_dsbuf, KIND_DSBUF, g_dsbuf_n);
    g_dsbuf_n++;
    RET(DD_OK);
    STDRET(4);
}

static void ds_DuplicateSoundBuffer(void) {
    if (ARG(2)) MEM32(ARG(2)) = ARG(1);
    RET(DD_OK);
    STDRET(3);
}

static void dsb_GetCaps(void) {
    dsbuf_t* b = dsbuf_of(ARG(0));
    uint32_t p = ARG(1);
    if (p) {
        MEM32(p) = 24;
        MEM32(p + 4) = 0;
        MEM32(p + 8) = b ? b->bytes : 0;
        MEM32(p + 12) = 0;
        MEM32(p + 16) = 0;
    }
    RET(DD_OK);
    STDRET(2);
}

/* Nothing advances, so a looping mixer sees a buffer that never drains. That
 * is the honest answer for silence and keeps the engine's timing sane. */
static void dsb_GetCurrentPosition(void) {
    if (ARG(1)) MEM32(ARG(1)) = 0;
    if (ARG(2)) MEM32(ARG(2)) = 0;
    RET(DD_OK);
    STDRET(3);
}

static void dsb_GetStatus(void) { if (ARG(1)) MEM32(ARG(1)) = 0; RET(DD_OK); STDRET(2); }
static void dsb_GetVolume(void) { if (ARG(1)) MEM32(ARG(1)) = 0; RET(DD_OK); STDRET(2); }
static void dsb_GetPan(void)    { if (ARG(1)) MEM32(ARG(1)) = 0; RET(DD_OK); STDRET(2); }
static void dsb_GetFrequency(void) { if (ARG(1)) MEM32(ARG(1)) = 22050; RET(DD_OK); STDRET(2); }

static void dsb_GetFormat(void) {
    uint32_t p = ARG(1);
    if (p && ARG(2) >= 18) {
        MEM16(p) = 1;                      /* WAVE_FORMAT_PCM */
        MEM16(p + 2) = 2;                  /* channels */
        MEM32(p + 4) = 22050;              /* samples/sec */
        MEM32(p + 8) = 22050 * 4;          /* avg bytes/sec */
        MEM16(p + 12) = 4;                 /* block align */
        MEM16(p + 14) = 16;                /* bits */
        MEM16(p + 16) = 0;
    }
    if (ARG(3)) MEM32(ARG(3)) = 18;
    RET(DD_OK);
    STDRET(4);
}

static void dsb_Lock(void) {
    dsbuf_t* b = dsbuf_of(ARG(0));
    uint32_t off = ARG(1), want = ARG(2);
    if (!b) { RET(DDERR_INVALIDPARAMS); STDRET(8); return; }
    if (off > b->bytes) off = 0;
    if (!want || off + want > b->bytes) want = b->bytes - off;
    if (ARG(3)) MEM32(ARG(3)) = b->mem + off;
    if (ARG(4)) MEM32(ARG(4)) = want;
    if (ARG(5)) MEM32(ARG(5)) = 0;         /* never wraps: one span is enough */
    if (ARG(6)) MEM32(ARG(6)) = 0;
    RET(DD_OK);
    STDRET(8);
}

static void dsb_Unlock(void) { RET(DD_OK); STDRET(5); }
static void dsb_Play(void)   { RET(DD_OK); STDRET(4); }
static void dsb_Stop(void)   { RET(DD_OK); STDRET(1); }
static void dsb_Restore(void) { RET(DD_OK); STDRET(1); }
static void dsb_SetCurrentPosition(void) { RET(DD_OK); STDRET(2); }
static void dsb_SetFormat(void) { RET(DD_OK); STDRET(2); }
static void dsb_SetVolume(void) { RET(DD_OK); STDRET(2); }
static void dsb_SetPan(void) { RET(DD_OK); STDRET(2); }
static void dsb_SetFrequency(void) { RET(DD_OK); STDRET(2); }
static void dsb_Initialize(void) { RET(DD_OK); STDRET(3); }

/* ============================================================
 * DirectPlay
 *
 * DPLAYX is imported by ordinal too (DirectPlayLobbyCreateA), and the engine
 * enumerates network providers during startup: create the lobby, query it for
 * IDirectPlay3A, call EnumConnections. With the stub returning failure,
 * SessionManager::EnumerateConnections dereferenced the null interface.
 *
 * This reports a machine with no network providers, which is both true here and
 * exactly what the engine is equipped to handle -- it just finds nothing to
 * list. Only the three methods on that path have bodies; every other slot is
 * null, so if the engine ever reaches one the dispatcher names the address
 * instead of guessing an argument count.
 * ============================================================ */

#define KIND_DPLAY 6

static void dp_QueryInterface(void);

static void dp_AddRef(void)  { RET(1); STDRET(1); }
static void dp_Release(void) { RET(0); STDRET(1); }

/* EnumConnections(this, pGuidApplication, lpEnumCallback, pvContext, dwFlags).
 * Returning DP_OK without ever invoking the callback is "no providers". */
static void dp_EnumConnections(void) {
    fprintf(stderr, "[dplay] EnumConnections -> no providers\n");
    RET(DD_OK);
    STDRET(5);
}

static uint32_t g_vt_dp3;

static void dp_QueryInterface(void) {
    if (!ARG(2)) { RET(DDERR_INVALIDPARAMS); STDRET(3); return; }
    fprintf(stderr, "[dplay] QueryInterface {%08X...} -> IDirectPlay3A\n",
            ARG(1) ? MEM32(ARG(1)) : 0);
    MEM32(ARG(2)) = new_object(g_vt_dp3, KIND_DPLAY, 0);
    RET(DD_OK);
    STDRET(3);
}

/* ---- vtable construction ------------------------------------------------ */

static uint32_t build_vtable(const recomp_func_t* fns, uint32_t n) {
    uint32_t vt = shims_alloc(n * 4);
    for (uint32_t i = 0; i < n; i++)
        MEM32(vt + i * 4) = com_slot(fns[i]);
    return vt;
}

uint32_t ddraw_create(void) {
    static const recomp_func_t surface_vt[] = {
        sf_QueryInterface, sf_AddRef, sf_Release,
        NULL, NULL,                       /* AddAttachedSurface, AddOverlayDirtyRect */
        sf_Blt, NULL, sf_BltFast,         /* Blt, BltBatch, BltFast */
        NULL, NULL, NULL,                 /* Delete/Enum attached, EnumOverlayZOrders */
        sf_Flip, sf_GetAttachedSurface,
        NULL, sf_GetCaps, NULL, NULL,     /* GetBltStatus, GetCaps, GetClipper, GetColorKey */
        sf_GetDC, NULL, NULL,             /* GetDC, GetFlipStatus, GetOverlayPosition */
        sf_GetPalette, sf_GetPixelFormat, sf_GetSurfaceDesc,
        NULL, sf_IsLost, sf_Lock, sf_ReleaseDC, sf_Restore,
        sf_SetClipper, sf_SetColorKey, NULL,
        sf_SetPalette, sf_Unlock,
        NULL, NULL, NULL,
    };
    static const recomp_func_t palette_vt[] = {
        pal_QueryInterface, pal_AddRef, pal_Release,
        pal_GetCaps, pal_GetEntries, pal_Initialize, pal_SetEntries,
    };
    static const recomp_func_t dd_vt[] = {
        dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact,
        dd_CreateClipper, dd_CreatePalette, dd_CreateSurface, NULL,
        NULL, NULL, dd_FlipToGDISurface, dd_GetCaps,
        dd_GetDisplayMode, NULL, NULL, NULL,
        NULL, NULL, NULL, dd_RestoreDisplayMode,
        dd_SetCooperativeLevel, dd_SetDisplayMode, dd_WaitForVerticalBlank,
    };

    if (!g_vt_surface) {
        g_vt_surface = build_vtable(surface_vt,
                                    sizeof surface_vt / sizeof *surface_vt);
        g_vt_palette = build_vtable(palette_vt,
                                    sizeof palette_vt / sizeof *palette_vt);
        for (int i = 0; i < 256; i++) {          /* a legible default ramp */
            g_pal[i].rgbRed = g_pal[i].rgbGreen = g_pal[i].rgbBlue = (BYTE)i;
        }
    }
    /* IDirectDraw2: identical up to SetCooperativeLevel, then a wider
     * SetDisplayMode and GetAvailableVidMem on the end. */
    static const recomp_func_t dd2_vt[] = {
        dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact,
        dd_CreateClipper, dd_CreatePalette, dd_CreateSurface, NULL,
        NULL, NULL, dd_FlipToGDISurface, dd_GetCaps,
        dd_GetDisplayMode, NULL, NULL, NULL,
        NULL, NULL, NULL, dd_RestoreDisplayMode,
        dd_SetCooperativeLevel, dd2_SetDisplayMode, dd_WaitForVerticalBlank,
        dd_GetAvailableVidMem,
    };
    if (!g_vt_dd2)
        g_vt_dd2 = build_vtable(dd2_vt, sizeof dd2_vt / sizeof *dd2_vt);

    uint32_t vt = build_vtable(dd_vt, sizeof dd_vt / sizeof *dd_vt);
    uint32_t obj = new_object(vt, KIND_DD, 0);
    fprintf(stderr, "[ddraw] DirectDrawCreate -> object 0x%08X\n", obj);
    return obj;
}

uint32_t dplay_lobby_create(void) {
    /* IDirectPlay3A: only EnumConnections (slot 35) is on the startup path.
     * Everything else stays null on purpose -- an unimplemented COM method has
     * no argument count we could guess, and a wrong STDRET would desync the
     * stack silently, whereas a null slot is reported by address. */
    static recomp_func_t dp3_vt[47];
    static recomp_func_t lobby_vt[14];

    if (!g_vt_dp3) {
        dp3_vt[0] = dp_QueryInterface;
        dp3_vt[1] = dp_AddRef;
        dp3_vt[2] = dp_Release;
        dp3_vt[35] = dp_EnumConnections;
        g_vt_dp3 = build_vtable(dp3_vt, 47);

        lobby_vt[0] = dp_QueryInterface;
        lobby_vt[1] = dp_AddRef;
        lobby_vt[2] = dp_Release;
    }
    uint32_t vt = build_vtable(lobby_vt, 14);
    uint32_t obj = new_object(vt, KIND_DPLAY, 0);
    fprintf(stderr, "[dplay] DirectPlayLobbyCreateA -> object 0x%08X\n", obj);
    return obj;
}

uint32_t dsound_create(void) {
    static const recomp_func_t dsbuf_vt[] = {
        ds_QueryInterface, ds_AddRef, ds_Release,
        dsb_GetCaps, dsb_GetCurrentPosition, dsb_GetFormat, dsb_GetVolume,
        dsb_GetPan, dsb_GetFrequency, dsb_GetStatus, dsb_Initialize,
        dsb_Lock, dsb_Play, dsb_SetCurrentPosition, dsb_SetFormat,
        dsb_SetVolume, dsb_SetPan, dsb_SetFrequency, dsb_Stop,
        dsb_Unlock, dsb_Restore,
    };
    static const recomp_func_t ds_vt[] = {
        ds_QueryInterface, ds_AddRef, ds_Release,
        ds_CreateSoundBuffer, ds_GetCaps, ds_DuplicateSoundBuffer,
        ds_SetCooperativeLevel, ds_Compact, ds_GetSpeakerConfig,
        ds_SetSpeakerConfig, ds_Initialize,
    };
    if (!g_vt_dsbuf)
        g_vt_dsbuf = build_vtable(dsbuf_vt, sizeof dsbuf_vt / sizeof *dsbuf_vt);
    uint32_t vt = build_vtable(ds_vt, sizeof ds_vt / sizeof *ds_vt);
    uint32_t obj = new_object(vt, KIND_DSOUND, 0);
    fprintf(stderr, "[dsound] DirectSoundCreate -> object 0x%08X (silent)\n", obj);
    return obj;
}
