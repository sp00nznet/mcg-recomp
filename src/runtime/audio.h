/*
 * Host audio: a software mixer on waveOut. DirectSound buffers (ddraw.c) are
 * voices; movies push PCM through streams.
 */
#ifndef MCG_AUDIO_H
#define MCG_AUDIO_H
#include <stdint.h>

/* One DirectSound secondary buffer as the mixer sees it. `data` is the
 * buffer's arena memory (host pointer), written by the game through Lock. */
typedef struct {
    const uint8_t* data;
    uint32_t bytes;
    int channels, bits, block, rate;
    int playing, looping;
    double pos;              /* play cursor, in frames */
    int volume, pan;         /* hundredths of a dB, as DirectSound */
    float gl, gr;
} mcg_voice;

#define MCG_MAX_VOICES 64
extern mcg_voice g_voice[MCG_MAX_VOICES];

void mcg_audio_init(void);   /* idempotent; starts the mixer thread */
void mcg_audio_lock(void);   /* around any change to g_voice */
void mcg_audio_unlock(void);
void mcg_voice_gains(mcg_voice* v);
int  mcg_audio_rate(void);   /* the mix rate */

int  mcg_audio_stream_open(int rate, int channels);   /* -1 if no audio */
void mcg_audio_stream_push(int id, const int16_t* pcm, int frames); /* interleaved s16 */
int  mcg_audio_stream_queued(int id);                 /* frames not yet played */
void mcg_audio_stream_close(int id);

#endif
