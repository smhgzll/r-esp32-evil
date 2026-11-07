// re1_audio.h - movie-audio hooks of the port's mixer (port/common/Audio.cpp),
// used by the FMV player (port/common/Video.cpp).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Open S16 PCM at `dataOffset` in `path` as the movie's audio stream.
int  re1_audio_fmv_open(const char* path, long dataOffset, int rate, int channels,
                        uint32_t frames);
// Start (or restart) playback at `fromFrame` (audio frames).
void re1_audio_fmv_play(uint32_t fromFrame);
void re1_audio_fmv_close(void);
// Audio frames played so far (the movie clock), -1 when not playing.
long re1_audio_fmv_pos(void);
int  re1_audio_fmv_rate(void);
// 1 when an output device is running (otherwise the movie uses wall time).
int  re1_audio_running(void);

#ifdef __cplusplus
}
#endif
