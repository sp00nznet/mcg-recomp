/*
 * Smacker (SMK2/SMK4) movies, and the smackw32.dll imports that play them.
 *
 * The decoder is written from the public description of the format (the
 * MultimediaWiki "Smacker" page): a 104-byte header, per-frame size and type
 * tables, four Huffman-coded "big trees" shared by every frame, then frames of
 * [palette][audio tracks][video]. Video is 4x4 blocks: MONO (two colours and a
 * bitmap), FULL (16 literal pixels), SKIP (keep the last frame) and FILL.
 * Audio is DPCM coded with small 8-bit Huffman trees per chunk.
 *
 * The engine (aSmackerWindow, Awindow.obj) drives it like this:
 *     SmackOpen(name, SMACKTRACKS, -1)
 *     SmackColorRemap(smk, gamepal, 256, 0)   -- 8-bit shell, or it takes
 *                                                smk->Palette itself
 *     each display():  SmackToBuffer(smk, x, y, pitch, h, buf, 0)
 *                      if (!SmackWait(smk)) { SmackDoFrame; SmackToBufferRect;
 *                          stop at FrameNum == Frames-1, else SmackNextFrame }
 *     SmackClose(smk)
 * and reads Width +4, Height +8, Frames +0xC, NewPalette +0x68, Palette +0x6C
 * (256 8-bit RGB triples) and FrameNum +0x374 straight out of the struct, so
 * the struct lives in simulated memory with exactly that layout.
 *
 * Build with -DSMK_TEST for a standalone check that decodes a movie to PPM
 * frames and a WAV (see the bottom of the file).
 */
#ifndef SMK_TEST
#include <windows.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Bit reader: Smacker packs LSB first.
 * ============================================================ */
typedef struct { const uint8_t* p; size_t n, pos; } bits_t;   /* pos in bits */

static unsigned get1(bits_t* b) {
    if (b->pos >= b->n * 8) { b->pos++; return 0; }
    unsigned v = (b->p[b->pos >> 3] >> (b->pos & 7)) & 1;
    b->pos++;
    return v;
}
static unsigned getn(bits_t* b, int n) {
    unsigned v = 0;
    for (int i = 0; i < n; i++) v |= get1(b) << i;
    return v;
}

/* ============================================================
 * Huffman trees. Nodes live in one array; a leaf has kid[0] < 0.
 * ============================================================ */
typedef struct { int kid[2]; int val; } hnode_t;
typedef struct { hnode_t* n; int count, cap; } htree_t;

static int hnew(htree_t* t) {
    if (t->count == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->n = (hnode_t*)realloc(t->n, (size_t)t->cap * sizeof *t->n);
    }
    t->n[t->count].kid[0] = t->n[t->count].kid[1] = -1;
    t->n[t->count].val = 0;
    return t->count++;
}

/* Walk one code. An empty tree decodes to 0 and reads nothing. */
static int hdecode(const htree_t* t, bits_t* b) {
    int i = 0;
    if (!t->count) return 0;
    while (t->n[i].kid[0] >= 0) {
        int k = t->n[i].kid[get1(b)];
        if (k < 0 || b->pos > b->n * 8 + 64) return 0;
        i = k;
    }
    return t->n[i].val;
}

/* 8-bit tree body: 1 = branch (0 side, then 1 side), 0 = leaf + 8 bits. */
static int read_small(htree_t* t, bits_t* b, int depth) {
    int me = hnew(t);
    if (depth > 32 || b->pos > b->n * 8) return me;
    if (get1(b)) {
        int k0 = read_small(t, b, depth + 1);
        t->n[me].kid[0] = k0;
        int k1 = read_small(t, b, depth + 1);
        t->n[me].kid[1] = k1;
    } else {
        t->n[me].val = (int)getn(b, 8);
    }
    return me;
}

/* Presence bit, then the tree and a closing 0 bit. */
static void read_small_tree(htree_t* t, bits_t* b) {
    t->count = 0;
    if (get1(b)) { read_small(t, b, 0); get1(b); }
}

/* Big (16-bit) tree: leaves are a low-tree code then a high-tree code. Leaves
 * whose value equals one of three escape codes become the three-entry recency
 * cache instead (val = -1 - slot). */
typedef struct { htree_t t; int esc[3]; int cache[3]; } bigtree_t;

static int read_big(bigtree_t* bt, bits_t* b, const htree_t* lo, const htree_t* hi,
                    int depth) {
    int me = hnew(&bt->t);
    if (depth > 64 || b->pos > b->n * 8) return me;
    if (get1(b)) {
        int k0 = read_big(bt, b, lo, hi, depth + 1);
        bt->t.n[me].kid[0] = k0;
        int k1 = read_big(bt, b, lo, hi, depth + 1);
        bt->t.n[me].kid[1] = k1;
    } else {
        int v = hdecode(lo, b);
        v |= hdecode(hi, b) << 8;
        for (int s = 0; s < 3; s++)
            if (v == bt->esc[s]) { v = -1 - s; break; }
        bt->t.n[me].val = v;
    }
    return me;
}

static void read_big_tree(bigtree_t* bt, bits_t* b) {
    htree_t lo = {0}, hi = {0};
    bt->t.count = 0;
    if (get1(b)) {
        read_small_tree(&lo, b);
        read_small_tree(&hi, b);
        for (int s = 0; s < 3; s++) bt->esc[s] = (int)getn(b, 16);
        read_big(bt, b, &lo, &hi, 0);
        get1(b);
    }
    free(lo.n); free(hi.n);
}

static int big_decode(bigtree_t* bt, bits_t* b) {
    int v = hdecode(&bt->t, b);
    if (v < 0) v = bt->cache[-1 - v];
    if (v != bt->cache[0]) {
        bt->cache[2] = bt->cache[1];
        bt->cache[1] = bt->cache[0];
        bt->cache[0] = v;
    }
    return v;
}

/* ============================================================
 * The movie.
 * ============================================================ */
typedef struct {
    uint8_t* file; size_t size;
    int w, h, frames, flags, ms_x100;      /* ms_x100: frame period * 100 */
    int is_smk4;
    uint32_t audio_rate[7];
    uint32_t* fsize; uint8_t* ftype; size_t* foff;
    bigtree_t mmap, mclr, full, type;
    uint8_t* video;                        /* w*h, persists between frames */
    uint8_t pal[768];
    int pal_changed;
    int16_t* pcm; int pcm_frames, pcm_cap; /* track 0 audio of the last frame */
    int pcm_rate, pcm_channels;
} smk_t;

static uint32_t rd32(const uint8_t* p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void smk_free(smk_t* m) {
    if (!m) return;
    free(m->file); free(m->fsize); free(m->ftype); free(m->foff);
    free(m->mmap.t.n); free(m->mclr.t.n); free(m->full.t.n); free(m->type.t.n);
    free(m->video); free(m->pcm); free(m);
}

static smk_t* smk_open(const char* path) {
    FILE* f = fopen(path, "rb");
    smk_t* m;
    if (!f) return NULL;
    m = (smk_t*)calloc(1, sizeof *m);
    fseek(f, 0, SEEK_END); m->size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    m->file = (uint8_t*)malloc(m->size ? m->size : 1);
    if (fread(m->file, 1, m->size, f) != m->size) m->size = 0;
    fclose(f);
    if (m->size < 104 || memcmp(m->file, "SMK", 3) ||
        (m->file[3] != '2' && m->file[3] != '4')) { smk_free(m); return NULL; }
    const uint8_t* h = m->file;
    m->is_smk4 = h[3] == '4';
    m->w = (int)rd32(h + 4);
    m->h = (int)rd32(h + 8);
    m->frames = (int)rd32(h + 12);
    int32_t rate = (int32_t)rd32(h + 16);
    m->ms_x100 = rate > 0 ? rate * 100 : rate < 0 ? -rate : 10000;
    m->flags = (int)rd32(h + 20);
    for (int i = 0; i < 7; i++) m->audio_rate[i] = rd32(h + 0x48 + 4 * i);
    uint32_t trees = rd32(h + 0x34);
    int n = m->frames + (m->flags & 1);    /* ring frame */
    size_t off = 104;
    if (m->w <= 0 || m->h <= 0 || m->w > 4096 || m->h > 4096 || n <= 0 ||
        off + (size_t)n * 5 + trees > m->size) { smk_free(m); return NULL; }
    m->fsize = (uint32_t*)malloc(n * sizeof(uint32_t));
    m->ftype = (uint8_t*)malloc(n);
    m->foff = (size_t*)malloc(n * sizeof(size_t));
    for (int i = 0; i < n; i++) m->fsize[i] = rd32(h + off + 4 * i) & ~3u;
    off += 4 * (size_t)n;
    memcpy(m->ftype, h + off, n);
    off += n;
    {
        bits_t b = { h + off, trees, 0 };
        read_big_tree(&m->mmap, &b);
        read_big_tree(&m->mclr, &b);
        read_big_tree(&m->full, &b);
        read_big_tree(&m->type, &b);
    }
    off += trees;
    for (int i = 0; i < n; i++) { m->foff[i] = off; off += m->fsize[i]; }
    m->video = (uint8_t*)calloc((size_t)m->w * m->h, 1);
    return m;
}

/* Interlaced and doubled movies are shown at twice their stored height. */
static int smk_out_h(const smk_t* m) { return (m->flags & 6) ? m->h * 2 : m->h; }

static void smk_palette(smk_t* m, const uint8_t* p, size_t len) {
    uint8_t old[768];
    size_t k = 0;
    memcpy(old, m->pal, 768);
    for (int i = 0; i < 256 && k < len;) {
        uint8_t c = p[k++];
        if (c & 0x80) {                    /* keep the next N entries */
            i += (c & 0x7F) + 1;
        } else if (c & 0x40) {             /* copy N entries from the old one */
            int cnt = (c & 0x3F) + 1;
            int src = k < len ? p[k++] : 0;
            while (cnt-- && i < 256 && src < 256) {
                memcpy(m->pal + 3 * i, old + 3 * src, 3); i++; src++;
            }
        } else {                           /* literal 6-bit RGB */
            uint8_t r = c & 0x3F, g = (k < len ? p[k++] : 0) & 0x3F,
                    bl = (k < len ? p[k++] : 0) & 0x3F;
            m->pal[3 * i + 0] = (uint8_t)((r * 255 + 31) / 63);
            m->pal[3 * i + 1] = (uint8_t)((g * 255 + 31) / 63);
            m->pal[3 * i + 2] = (uint8_t)((bl * 255 + 31) / 63);
            i++;
        }
    }
    m->pal_changed = 1;
}

/* Decode one audio chunk of the track whose header rate word is `ar` and
 * append it to m->pcm as s16. Only DPCM and raw PCM; the Bink-audio variants
 * (bits 26/27) never appear in MechCommander's movies and come out silent. */
static void smk_audio(smk_t* m, uint32_t ar, const uint8_t* p, size_t len) {
    int stereo = (ar >> 28) & 1, is16 = (ar >> 29) & 1;
    int ch = stereo + 1;
    m->pcm_rate = (int)(ar & 0xFFFFFF);
    m->pcm_channels = ch;
    if (ar & 0x0C000000u) return;
    if (!(ar & 0x80000000u)) {             /* uncompressed */
        int bps = is16 ? 2 : 1;
        for (size_t i = 0; i + bps <= len; i += bps) {
            int v = is16 ? (int16_t)(p[i] | p[i + 1] << 8) : (p[i] - 128) << 8;
            int need = (m->pcm_frames + 1) * ch;
            if (need > m->pcm_cap) {
                m->pcm_cap = need * 2;
                m->pcm = (int16_t*)realloc(m->pcm, (size_t)m->pcm_cap * 2);
            }
            m->pcm[m->pcm_frames * ch + (int)((i / bps) % ch)] = (int16_t)v;
            if ((int)((i / bps) % ch) == ch - 1) m->pcm_frames++;
        }
        return;
    }
    if (len < 4) return;
    uint32_t unpacked = rd32(p);
    bits_t b = { p + 4, len - 4, 0 };
    if (!get1(&b)) return;                 /* no data in this chunk */
    stereo = (int)get1(&b);
    is16 = (int)get1(&b);
    ch = stereo + 1;
    m->pcm_channels = ch;
    htree_t t[4] = {{0}};
    int nt = ch * (is16 ? 2 : 1);
    for (int i = 0; i < nt; i++) read_small_tree(&t[i], &b);
    int pred[2] = {0, 0};
    /* Starting values, right channel first; 16-bit ones high byte first. */
    for (int c = stereo; c >= 0; c--) {
        if (is16) { int hb = (int)getn(&b, 8); pred[c] = (int16_t)(hb << 8 | getn(&b, 8)); }
        else pred[c] = (int)getn(&b, 8);
    }
    int total = (int)(unpacked / (is16 ? 2 : 1));  /* samples, all channels */
    if (total > 8 * 1024 * 1024) total = 0;
    int need = (m->pcm_frames + total / ch + 1) * ch;
    if (need > m->pcm_cap) {
        m->pcm_cap = need;
        m->pcm = (int16_t*)realloc(m->pcm, (size_t)m->pcm_cap * 2);
    }
    int16_t* out = m->pcm + m->pcm_frames * ch;
    for (int s = 0; s < total; s++) {
        int c = s % ch;
        if (s >= ch) {
            if (is16) {
                int lo = hdecode(&t[2 * c], &b), hi = hdecode(&t[2 * c + 1], &b);
                pred[c] = (int16_t)(pred[c] + (int16_t)(lo | hi << 8));
            } else {
                pred[c] = (uint8_t)(pred[c] + (int8_t)hdecode(&t[c], &b));
            }
        }
        out[s] = (int16_t)(is16 ? pred[c] : (pred[c] - 128) << 8);
    }
    m->pcm_frames += total / ch;
    for (int i = 0; i < nt; i++) free(t[i].n);
}

static const int k_runs[64] = {
     1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
    49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 128, 256, 512, 1024, 2048 };

static void smk_video(smk_t* m, const uint8_t* p, size_t len) {
    bits_t b = { p, len, 0 };
    int bw = m->w / 4, bh = m->h / 4, nblk = bw * bh, blk = 0;
    bigtree_t* trees[4] = { &m->mmap, &m->mclr, &m->full, &m->type };
    for (int i = 0; i < 4; i++) memset(trees[i]->cache, 0, sizeof trees[i]->cache);
    while (blk < nblk) {
        int v = big_decode(&m->type, &b);
        int kind = v & 3, run = k_runs[(v >> 2) & 0x3F], data = (v >> 8) & 0xFF;
        while (run-- && blk < nblk) {
            uint8_t* o = m->video + (size_t)(blk / bw) * 4 * m->w + (blk % bw) * 4;
            int pitch = m->w;
            switch (kind) {
            case 0: {                                  /* MONO */
                int clr = big_decode(&m->mclr, &b);
                int map = big_decode(&m->mmap, &b);
                uint8_t hi = (uint8_t)(clr >> 8), lo = (uint8_t)clr;
                for (int y = 0; y < 4; y++, o += pitch)
                    for (int x = 0; x < 4; x++, map >>= 1) o[x] = (map & 1) ? hi : lo;
                break;
            }
            case 1: {                                  /* FULL */
                int mode = 0;
                if (m->is_smk4) { if (get1(&b)) mode = 1; else if (get1(&b)) mode = 2; }
                if (mode == 0) {
                    for (int y = 0; y < 4; y++, o += pitch) {
                        int a = big_decode(&m->full, &b);
                        o[2] = (uint8_t)a; o[3] = (uint8_t)(a >> 8);
                        a = big_decode(&m->full, &b);
                        o[0] = (uint8_t)a; o[1] = (uint8_t)(a >> 8);
                    }
                } else if (mode == 1) {                /* 2x2 doubled pixels */
                    for (int y = 0; y < 2; y++, o += 2 * pitch) {
                        int a = big_decode(&m->full, &b);
                        o[0] = o[1] = o[pitch] = o[pitch + 1] = (uint8_t)a;
                        o[2] = o[3] = o[pitch + 2] = o[pitch + 3] = (uint8_t)(a >> 8);
                    }
                } else {                               /* rows doubled */
                    for (int y = 0; y < 2; y++, o += 2 * pitch) {
                        int a = big_decode(&m->full, &b), c = big_decode(&m->full, &b);
                        o[0] = o[pitch] = (uint8_t)c; o[1] = o[pitch + 1] = (uint8_t)(c >> 8);
                        o[2] = o[pitch + 2] = (uint8_t)a; o[3] = o[pitch + 3] = (uint8_t)(a >> 8);
                    }
                }
                break;
            }
            case 2: break;                             /* SKIP */
            default:                                   /* FILL */
                for (int y = 0; y < 4; y++, o += pitch) memset(o, data, 4);
                break;
            }
            blk++;
        }
    }
}

/* Decode frame i: palette, track 0 audio into m->pcm (replacing the last
 * frame's), video into m->video. Frames must be decoded in order. */
static void smk_decode(smk_t* m, int i) {
    int n = m->frames + (m->flags & 1);
    m->pal_changed = 0;
    m->pcm_frames = 0;
    if (i < 0 || i >= n) return;
    const uint8_t* p = m->file + m->foff[i];
    size_t left = m->fsize[i];
    if (m->foff[i] + left > m->size) left = m->foff[i] < m->size ? m->size - m->foff[i] : 0;
    if ((m->ftype[i] & 1) && left) {
        size_t len = (size_t)p[0] * 4;
        if (len > left) len = left;
        if (len) smk_palette(m, p + 1, len - 1);
        p += len; left -= len;
    }
    for (int t = 0; t < 7; t++) {
        if (!(m->ftype[i] & (2 << t))) continue;
        if (left < 4) return;
        size_t len = rd32(p);
        if (len < 4 || len > left) return;
        if (t == 0) smk_audio(m, m->audio_rate[0], p + 4, len - 4);
        p += len; left -= len;
    }
    smk_video(m, p, left);
}

/* Copy the video into an 8-bit destination, doubling rows if the movie asks. */
static void smk_blit(const smk_t* m, uint8_t* dst, int pitch, int maxh,
                     const uint8_t* remap) {
    int oh = smk_out_h(m);
    for (int y = 0; y < oh && y < maxh; y++) {
        const uint8_t* s = m->video + (size_t)((m->flags & 6) ? y / 2 : y) * m->w;
        uint8_t* d = dst + (size_t)y * pitch;
        if (remap) for (int x = 0; x < m->w; x++) d[x] = remap[s[x]];
        else memcpy(d, s, m->w);
    }
}

#ifndef SMK_TEST
/* ============================================================
 * The smackw32 imports.
 * ============================================================ */
#include "recomp_types.h"
#include "imports.h"
#include "audio.h"

uint32_t shims_alloc(uint32_t n);

#define SMK_STRUCT   0x400u    /* covers every field through FrameNum (+0x374) */
#define S_WIDTH      0x04
#define S_HEIGHT     0x08
#define S_FRAMES     0x0C
#define S_MSPERFRAME 0x10
#define S_TYPE       0x14
#define S_NEWPAL     0x68
#define S_PALETTE    0x6C
#define S_FRAMENUM   0x374

typedef struct {
    uint32_t va;               /* the Smack struct in simulated memory */
    smk_t* m;
    uint32_t buf, left, top, pitch, height, bufflags;
    int have_remap;
    uint8_t remap_pal[768];
    uint8_t remap[256];
    int audio;                 /* host stream id, or -1 */
    double due;                /* when the next frame may be shown, ms */
} player_t;

#define MAX_PLAYERS 8
static player_t g_play[MAX_PLAYERS];

static double now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static player_t* find(uint32_t va) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (va && g_play[i].va == va && g_play[i].m) return &g_play[i];
    return NULL;
}

static void build_remap(player_t* p) {
    const uint8_t* src = p->m->pal;
    for (int i = 0; i < 256; i++) {
        int best = 0, bd = 1 << 30;
        for (int j = 0; j < 256; j++) {
            int dr = src[3 * i] - p->remap_pal[3 * j];
            int dg = src[3 * i + 1] - p->remap_pal[3 * j + 1];
            int db = src[3 * i + 2] - p->remap_pal[3 * j + 2];
            int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bd) { bd = d; best = j; if (!d) break; }
        }
        p->remap[i] = (uint8_t)best;
    }
}

static void publish_palette(player_t* p) {
    memcpy((void*)ADDR(p->va + S_PALETTE), p->m->pal, 768);
    MEM32(p->va + S_NEWPAL) = p->m->pal_changed ? 1 : 0;
}

/* Smack* SmackOpen(const char* name, u32 flags, u32 extrabuf) */
void imp_SmackOpen(void) {
    char tmp[MAX_PATH];
    const char* name = (const char*)(uintptr_t)ADDR(ARG(0));
    player_t* p = NULL;
    smk_t* m;
    uint32_t va = 0;
    snprintf(tmp, sizeof tmp, "%s", name ? name : "");
    m = smk_open(tmp);
    for (int i = 0; m && i < MAX_PLAYERS; i++)
        if (!g_play[i].m) { p = &g_play[i]; break; }
    if (m && p) {
        uint32_t keep = p->va;             /* the arena never frees; reuse */
        memset(p, 0, sizeof *p);
        p->va = keep ? keep : shims_alloc(SMK_STRUCT);
        p->m = m;
        p->audio = -1;
        va = p->va;
        memset((void*)ADDR(va), 0, SMK_STRUCT);
        MEM32(va + 0) = m->is_smk4 ? 0x344B4D53u : 0x324B4D53u;
        MEM32(va + S_WIDTH) = (uint32_t)m->w;
        MEM32(va + S_HEIGHT) = (uint32_t)smk_out_h(m);
        MEM32(va + S_FRAMES) = (uint32_t)m->frames;
        MEM32(va + S_MSPERFRAME) = (uint32_t)(m->ms_x100 / 100);
        MEM32(va + S_TYPE) = (uint32_t)m->flags;
        /* Frame 0's palette is known before it is shown, as with the real
         * library: decode it alone from the same (black) starting palette
         * that SmackDoFrame will use. */
        if ((m->ftype[0] & 1) && m->fsize[0]) {
            const uint8_t* fp = m->file + m->foff[0];
            size_t len = (size_t)fp[0] * 4;
            if (len > 1 && len <= m->fsize[0]) {
                smk_palette(m, fp + 1, len - 1);
                publish_palette(p);
                memset(m->pal, 0, 768);
            }
        }
        p->due = now_ms();
    } else if (m) {
        smk_free(m);
        m = NULL;
    }
    fprintf(stderr, "[smack] SmackOpen(%s) -> %08X%s\n", tmp, va,
            m ? "" : " (failed)");
    if (m) fprintf(stderr, "[smack]   %dx%d, %d frames, %.2f ms/frame, flags %d, "
                   "audio %08X\n", m->w, smk_out_h(m), m->frames, m->ms_x100 / 100.0,
                   m->flags, m->audio_rate[0]);
    RET(va);
    STDRET(3);
}

/* void SmackClose(Smack*) */
void imp_SmackClose(void) {
    player_t* p = find(ARG(0));
    if (p) {
        if (p->audio >= 0) mcg_audio_stream_close(p->audio);
        smk_free(p->m);
        p->m = NULL;
    }
    RET(0);
    STDRET(1);
}

/* void SmackToBuffer(Smack*, u32 left, u32 top, u32 pitch, u32 height,
 *                    void* buf, u32 flags) -- where SmackDoFrame draws. */
void imp_SmackToBuffer(void) {
    player_t* p = find(ARG(0));
    if (p) {
        p->left = ARG(1); p->top = ARG(2); p->pitch = ARG(3);
        p->height = ARG(4); p->buf = ARG(5); p->bufflags = ARG(6);
        { static int said; if (!said) { said = 1;
            fprintf(stderr, "[smack] SmackToBuffer(%u,%u pitch %u h %u buf %08X "
                    "flags %X)\n", p->left, p->top, p->pitch, p->height, p->buf,
                    ARG(6)); } }
    }
    RET(0);
    STDRET(7);
}

/* void SmackColorRemap(Smack*, const void* pal, u32 numcolors, u32 paltype)
 * The engine's own palette (8-bit RGB triples); the movie's colours are
 * mapped to their nearest entry from now on. */
void imp_SmackColorRemap(void) {
    player_t* p = find(ARG(0));
    if (p && ARG(1)) {
        uint32_t n = ARG(2) > 256 ? 256 : ARG(2);
        memset(p->remap_pal, 0, 768);
        memcpy(p->remap_pal, (const void*)ADDR(ARG(1)), n * 3);
        p->have_remap = 1;
        /* The palette of the frame about to be shown, if it is known yet. */
        {
            uint8_t save[768];
            memcpy(save, p->m->pal, 768);
            memcpy(p->m->pal, (const void*)ADDR(p->va + S_PALETTE), 768);
            build_remap(p);
            memcpy(p->m->pal, save, 768);
        }
    }
    RET(0);
    STDRET(4);
}

/* u32 SmackDoFrame(Smack*): decode the current frame and draw it. */
void imp_SmackDoFrame(void) {
    player_t* p = find(ARG(0));
    if (p) {
        int fn = (int)MEM32(p->va + S_FRAMENUM);
        smk_t* m = p->m;
        smk_decode(m, fn);
        if (m->pal_changed) {
            publish_palette(p);
            if (p->have_remap) build_remap(p);
        } else {
            MEM32(p->va + S_NEWPAL) = 0;
        }
        if (m->pcm_frames) {
            if (p->audio < 0 && m->pcm_rate)
                p->audio = mcg_audio_stream_open(m->pcm_rate, m->pcm_channels);
            if (p->audio >= 0)
                mcg_audio_stream_push(p->audio, m->pcm, m->pcm_frames);
        }
        if (p->buf && (p->bufflags & 0x80000000u)) {   /* 16bpp surface */
            int oh = smk_out_h(m), maxh = (int)p->height;
            const uint8_t* pal = m->pal;
            for (int y = 0; y < oh && y < maxh; y++) {
                const uint8_t* sp = m->video + (size_t)((m->flags & 6) ? y / 2 : y) * m->w;
                uint16_t* d = (uint16_t*)((uint8_t*)(uintptr_t)ADDR(p->buf) +
                              (size_t)(p->top + y) * p->pitch + p->left * 2);
                for (int x = 0; x < m->w; x++) {
                    const uint8_t* c = pal + 3 * sp[x];
                    d[x] = (p->bufflags & 0x40000000u)
                        ? (uint16_t)((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3)
                        : (uint16_t)((c[0] >> 3) << 10 | (c[1] >> 3) << 5 | c[2] >> 3);
                }
            }
        } else if (p->buf) {
            uint8_t* dst = (uint8_t*)(uintptr_t)ADDR(p->buf) +
                           (size_t)p->top * p->pitch + p->left;
            smk_blit(m, dst, (int)p->pitch,
                     p->height > p->top ? (int)(p->height - p->top) : 0,
                     p->have_remap ? p->remap : NULL);
        }
        /* Pace to the movie's clock; resync after a long stall (a load). */
        double t = now_ms();
        p->due += m->ms_x100 / 100.0;
        if (p->due < t - 250.0) p->due = t;
    }
    RET(0);
    STDRET(1);
}

/* u32 SmackToBufferRect(Smack*, u32 surface): no dirty rectangle to report --
 * SmackDoFrame already drew the whole frame. */
void imp_SmackToBufferRect(void) {
    RET(0);
    STDRET(2);
}

/* void SmackNextFrame(Smack*) */
void imp_SmackNextFrame(void) {
    player_t* p = find(ARG(0));
    if (p) MEM32(p->va + S_FRAMENUM) += 1;
    RET(0);
    STDRET(1);
}

/* u32 SmackWait(Smack*): nonzero while it is too early for the next frame.
 * If audio is queued far ahead (more than half a second), wait on it too. */
void imp_SmackWait(void) {
    player_t* p = find(ARG(0));
    uint32_t wait = 0;
    if (p) {
        wait = now_ms() < p->due;
        if (!wait && p->audio >= 0 && p->m->pcm_rate &&
            mcg_audio_stream_queued(p->audio) > p->m->pcm_rate / 2 * 3)
            wait = 1;  /* ponytail: Smacker front-loads ~1s of audio; 1.5s cap */
    }
    RET(wait);
    STDRET(1);
}

/* u32 SmackDDSurfaceType(LPDIRECTDRAWSURFACE): the buffer flag SmackToBuffer
 * is then passed. The engine asks only in its 16bpp mode (it clears the
 * surface as 640x480x2 bytes), and the mock's 16bpp surfaces are 565, so
 * SMACKBUFFER565. Returning -1 makes the engine fault. */
void imp_SmackDDSurfaceType(void) {
    RET(0xC0000000u);
    STDRET(1);
}

/* u8 SmackSoundUseDirectSound(LPDIRECTSOUND): audio goes to the host stream
 * instead, so just say yes. */
void imp_SmackSoundUseDirectSound(void) {
    RET(1);
    STDRET(1);
}

#else
/* ============================================================
 * Standalone check: cl /DSMK_TEST smacker.c && smacker MOVIE.SMK outdir
 * Writes frame_NNNN.ppm every 30 frames, audio.wav, and prints a sanity line.
 * ============================================================ */
int main(int argc, char** argv) {
    char path[512];
    if (argc < 3) { fprintf(stderr, "usage: smacker in.smk outdir\n"); return 2; }
    smk_t* m = smk_open(argv[1]);
    if (!m) { fprintf(stderr, "cannot open\n"); return 1; }
    int oh = smk_out_h(m);
    uint8_t* img = (uint8_t*)malloc((size_t)m->w * oh);
    snprintf(path, sizeof path, "%s/audio.wav", argv[2]);
    FILE* wav = fopen(path, "wb");
    long samples = 0; int rate = 22050, chans = 1;
    fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\1\0\1\0\0\0\0\0\0\0\0\0\2\0\x10\0data\0\0\0\0",
           1, 44, wav);
    double diff = 0;
    for (int i = 0; i < m->frames; i++) {
        smk_decode(m, i);
        if (m->pcm_frames) {
            rate = m->pcm_rate; chans = m->pcm_channels;
            fwrite(m->pcm, 2, (size_t)m->pcm_frames * chans, wav);
            for (int s = 1; s < m->pcm_frames * chans; s++)
                diff += abs(m->pcm[s] - m->pcm[s - 1]);
            samples += m->pcm_frames;
        }
        if (i % 30 == 0) {
            smk_blit(m, img, m->w, oh, NULL);
            snprintf(path, sizeof path, "%s/frame_%04d.ppm", argv[2], i);
            FILE* f = fopen(path, "wb");
            fprintf(f, "P6\n%d %d\n255\n", m->w, oh);
            for (int k = 0; k < m->w * oh; k++) fwrite(m->pal + 3 * img[k], 1, 3, f);
            fclose(f);
        }
    }
    uint32_t data = (uint32_t)(samples * chans * 2), riff = data + 36;
    uint32_t br = (uint32_t)(rate * chans * 2);
    fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav);
    fseek(wav, 22, SEEK_SET); fwrite(&chans, 2, 1, wav); fwrite(&rate, 4, 1, wav);
    fwrite(&br, 4, 1, wav);
    fseek(wav, 40, SEEK_SET); fwrite(&data, 4, 1, wav);
    fclose(wav);
    /* Real audio is smooth: mean |delta| far below white noise (~21000). */
    double md = samples > 1 ? diff / (double)(samples * chans) : 0;
    printf("%dx%d %d frames, %ld audio frames @%d, mean |delta| %.0f\n",
           m->w, oh, m->frames, samples, rate, md);
    smk_free(m);
    return (samples && md > 8000) ? 1 : 0;
}
#endif
