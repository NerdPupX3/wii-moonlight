#ifndef AUDIO_H
#define AUDIO_H

#include "Limelight.h"

// Fills in the audio renderer callbacks so moonlight-common-c can hand us
// Opus packets. Decoded PCM is played through the Wii's ASND sound library.
void audio_init_callbacks(PAUDIO_RENDERER_CALLBACKS cb);

// Stops the audio hardware. Call before the app exits to the loader.
void audio_shutdown(void);

#endif
