/*
 * audio.c - the host side of DirectSound: a software mixer on waveOut.
 *
 * The design is pcrecomp's win32hle/dsound.c (SDL2) moved onto waveOut, so it
 * needs nothing Windows doesn't ship. A voice is a DirectSound buffer whose
 * memory the game Locks and writes; the mixer reads it at the buffer's rate,
 * volume and pan and moves its play cursor in real time, so the engine's own
 * streaming (it watches GetCurrentPosition) keeps working.
 *
 * The mixer runs on its own thread and only ever READS arena memory and the
 * voice table. It never calls lifted code, so the main-thread rule (registers
 * and stack are global) is not touched.
 *
 * With --headless, MCG_MUTE=1 or no wave device, the same thread still runs
 * the mix against the clock and throws it away: cursors move as they would
 * on a real card, so the engine's timing is the same either way.
 *
 * mcg.ini [sound] master_volume=0..100 and mute=1; MCG_VOLUME and MCG_MUTE
 * override them.
 * MCG_WAVDUMP=file.wav writes everything mixed to a WAV, device or not: the
 * way to hear a headless run.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "settings.h"

#define RATE    44100
#define BLOCK   1024              /* frames per waveOut buffer, ~23 ms */
#define NBUF    4                 /* ~93 ms queued */

extern int g_headless;

mcg_voice g_voice[MCG_MAX_VOICES];
static CRITICAL_SECTION g_cs;
static volatile LONG g_started;
static float g_master = 1.0f;
static FILE* g_dump;
static uint32_t g_dump_bytes;

static void dump_header(void) {
    uint32_t h[11] = { 0x46464952u, 36 + g_dump_bytes, 0x45564157u, 0x20746D66u, 16,
                       0x00020001u, RATE, RATE * 4, 0x00100004u, 0x61746164u, g_dump_bytes };
    fseek(g_dump, 0, SEEK_SET);
    fwrite(h, 4, 11, g_dump);
    fseek(g_dump, 0, SEEK_END);
    fflush(g_dump);
}

void mcg_audio_lock(void)   { mcg_audio_init(); EnterCriticalSection(&g_cs); }
void mcg_audio_unlock(void) { LeaveCriticalSection(&g_cs); }
int  mcg_audio_rate(void)   { return RATE; }

void mcg_voice_gains(mcg_voice* v) {
    float g = v->volume <= -10000 ? 0.0f : powf(10.0f, (float)v->volume / 2000.0f);
    float l = 1.0f, r = 1.0f;
    if (v->pan < 0) r = v->pan <= -10000 ? 0.0f : powf(10.0f, (float)v->pan / 2000.0f);
    if (v->pan > 0) l = v->pan >= 10000 ? 0.0f : powf(10.0f, (float)-v->pan / 2000.0f);
    v->gl = g * l, v->gr = g * r;
}

static float sample(const mcg_voice* v, uint32_t frame, int ch) {
    const uint8_t* p = v->data + (size_t)frame * (uint32_t)v->block
                     + (v->channels > 1 ? (size_t)ch * (uint32_t)(v->bits / 8) : 0);
    return v->bits == 16 ? (float)*(const int16_t*)p / 32768.0f : ((float)*p - 128.0f) / 128.0f;
}

/* ---- streams: PCM a host pushes (a movie's sound) ---- */
#define MAX_STREAMS 4
#define STREAM_FRAMES (1 << 17)
static struct { int used, channels, rate; int16_t* ring; uint32_t rd, wr; double pos; } g_st[MAX_STREAMS];

int mcg_audio_stream_open(int rate, int channels) {
    int id = -1;
    mcg_audio_lock();
    for (int i = 0; i < MAX_STREAMS && id < 0; i++)
        if (!g_st[i].used) {
            g_st[i].ring = (int16_t*)calloc(STREAM_FRAMES * 2, sizeof(int16_t));
            if (!g_st[i].ring) break;
            g_st[i].used = 1, g_st[i].channels = channels, g_st[i].rate = rate;
            g_st[i].rd = g_st[i].wr = 0, g_st[i].pos = 0;
            id = i;
        }
    mcg_audio_unlock();
    return id;
}
void mcg_audio_stream_push(int id, const int16_t* pcm, int frames) {
    if (id < 0 || id >= MAX_STREAMS) return;
    mcg_audio_lock();
    for (int f = 0; f < frames && g_st[id].wr - g_st[id].rd < STREAM_FRAMES - 1; f++, g_st[id].wr++) {
        uint32_t k = g_st[id].wr % STREAM_FRAMES;
        g_st[id].ring[2 * k]     = pcm[f * g_st[id].channels];
        g_st[id].ring[2 * k + 1] = pcm[f * g_st[id].channels + (g_st[id].channels > 1)];
    }
    mcg_audio_unlock();
}
int mcg_audio_stream_queued(int id) {
    if (id < 0 || id >= MAX_STREAMS) return 0;
    mcg_audio_lock();
    int n = (int)(g_st[id].wr - g_st[id].rd);
    mcg_audio_unlock();
    return n;
}
void mcg_audio_stream_close(int id) {
    if (id < 0 || id >= MAX_STREAMS) return;
    mcg_audio_lock();
    free(g_st[id].ring);
    memset(&g_st[id], 0, sizeof g_st[id]);
    mcg_audio_unlock();
}

/* ---- the mix ---- */
static void mix(int16_t* out, int frames) {
    static float acc[2 * BLOCK];
    memset(acc, 0, sizeof(float) * 2 * (size_t)frames);
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MCG_MAX_VOICES; i++) {
        mcg_voice* v = &g_voice[i];
        if (!v->playing || !v->block || !v->data) continue;
        uint32_t n = v->bytes / (uint32_t)v->block;
        double step = (double)v->rate / RATE;
        for (int f = 0; f < frames && v->playing; f++) {
            uint32_t at = (uint32_t)v->pos;
            if (at >= n) {
                if (v->looping && n) { v->pos = fmod(v->pos, n); at = (uint32_t)v->pos; }
                else { v->playing = 0; v->pos = 0; break; }
            }
            float l = sample(v, at, 0), r = v->channels > 1 ? sample(v, at, 1) : l;
            acc[2 * f] += l * v->gl;
            acc[2 * f + 1] += r * v->gr;
            v->pos += step;
        }
    }
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!g_st[i].used) continue;
        double step = (double)g_st[i].rate / RATE;
        for (int f = 0; f < frames && g_st[i].rd < g_st[i].wr; f++) {
            uint32_t k = g_st[i].rd % STREAM_FRAMES;
            acc[2 * f]     += g_st[i].ring[2 * k] / 32768.0f;
            acc[2 * f + 1] += g_st[i].ring[2 * k + 1] / 32768.0f;
            for (g_st[i].pos += step; g_st[i].pos >= 1.0 && g_st[i].rd < g_st[i].wr; g_st[i].pos -= 1.0) g_st[i].rd++;
        }
    }
    LeaveCriticalSection(&g_cs);
    static int16_t scratch[2 * BLOCK];
    if (!out) { if (!g_dump) return; out = scratch; }
    for (int k = 0; k < 2 * frames; k++) {
        float s = acc[k] * g_master * 32767.0f;
        out[k] = (int16_t)(s > 32767.0f ? 32767 : s < -32768.0f ? -32768 : s);
    }
    if (g_dump) {
        fwrite(out, 4, (size_t)frames, g_dump);
        g_dump_bytes += 4u * (uint32_t)frames;
        if ((g_dump_bytes & 0xFFFFF) < 4u * (uint32_t)frames) dump_header();   /* ~every MB: playable if killed */
    }
}

static DWORD WINAPI mixer_thread(LPVOID unused) {
    (void)unused;
    HWAVEOUT wo = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    const char* mute = getenv("MCG_MUTE");
    int muted = mute && *mute ? *mute == '1' : mcg_setting_int("sound", "mute", 0) != 0;
    if (!g_headless && !muted) {
        WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0 };
        if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) wo = NULL;
    }
    fprintf(stderr, "[audio] %s, %d Hz\n", wo ? "waveOut" : "no device (mixing against the clock)", RATE);

    if (wo) {
        static int16_t pcm[NBUF][2 * BLOCK];
        WAVEHDR h[NBUF];
        memset(h, 0, sizeof h);
        for (int i = 0; i < NBUF; i++) {
            h[i].lpData = (LPSTR)pcm[i];
            h[i].dwBufferLength = sizeof pcm[i];
            waveOutPrepareHeader(wo, &h[i], sizeof h[i]);
            mix(pcm[i], BLOCK);
            waveOutWrite(wo, &h[i], sizeof h[i]);
        }
        for (;;) {
            WaitForSingleObject(ev, 100);
            for (int i = 0; i < NBUF; i++)
                if (h[i].dwFlags & WHDR_DONE) {
                    mix(pcm[i], BLOCK);
                    waveOutWrite(wo, &h[i], sizeof h[i]);
                }
        }
    }
    /* No device: advance by the clock, ~one block at a time. */
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    long long done = 0;
    for (;;) {
        Sleep(10);
        QueryPerformanceCounter(&t);
        long long due = (t.QuadPart - t0.QuadPart) * RATE / f.QuadPart;
        while (due - done > 0) {
            int n = due - done > BLOCK ? BLOCK : (int)(due - done);
            mix(NULL, n);
            done += n;
        }
    }
}

void mcg_audio_init(void) {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;
    InitializeCriticalSection(&g_cs);
    const char* vol = getenv("MCG_VOLUME");
    g_master = (float)(vol && *vol ? atoi(vol) : mcg_setting_int("sound", "master_volume", 100)) / 100.0f;
    const char* dump = getenv("MCG_WAVDUMP");
    if (dump && *dump && (g_dump = fopen(dump, "wb")) != NULL) dump_header();
    CloseHandle(CreateThread(NULL, 0, mixer_thread, NULL, 0, NULL));
}
