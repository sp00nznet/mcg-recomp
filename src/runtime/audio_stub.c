/* TEMPORARY: no-op host audio until the real DirectSound/host-audio work
 * lands with its own audio.c. Delete this file (and its CMakeLists.txt line)
 * then. */
#include "audio.h"

int  mcg_audio_stream_open(int rate, int channels) { (void)rate; (void)channels; return -1; }
void mcg_audio_stream_push(int id, const int16_t* pcm, int frames) { (void)id; (void)pcm; (void)frames; }
int  mcg_audio_stream_queued(int id) { (void)id; return 0; }
void mcg_audio_stream_close(int id) { (void)id; }
