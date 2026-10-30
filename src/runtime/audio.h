/*
 * Host audio streams: interleaved s16 PCM pushed from the runtime and played
 * by the host. mcg_audio_stream_open returns -1 when there is no audio.
 */
#ifndef MCG_AUDIO_H
#define MCG_AUDIO_H

#include <stdint.h>

int  mcg_audio_stream_open(int rate, int channels);   /* -1 if no audio */
void mcg_audio_stream_push(int id, const int16_t* pcm, int frames); /* interleaved s16 */
int  mcg_audio_stream_queued(int id);                 /* frames not yet played */
void mcg_audio_stream_close(int id);

#endif
