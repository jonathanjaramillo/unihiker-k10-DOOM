#pragma once
// Generic PCM voice mixer over I2S, for the NS4168 amp on the UNIHIKER K10.
//
// Deliberately knows nothing about Doom or WAD lumps -- kept in its own
// translation unit (Arduino.h + FreeRTOS + the I2S driver) so it never has to
// be included alongside doomtype.h. Arduino.h and doomtype.h both typedef
// `boolean` to different underlying types; mixing them in one file is a hard
// compile error. k10_sound_module.c is the thin doom-facing glue on the
// other side of this boundary.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up the I2S peripheral, the amp, and the mixer task. Returns 1 on
// success, 0 on failure. Safe to call more than once (a no-op after the
// first success).
int k10_sound_init(void);

// Starts playing 8-bit unsigned PCM `data` (length `samples`, `srcRate` Hz)
// on the given voice channel, replacing whatever was playing there. `data`
// must stay valid until the voice stops (the mixer does not copy it -- it's
// expected to be WAD-cache memory that outlives the sound). volL/volR are
// linear gains in 0..127. Returns the channel index, or -1 on failure.
int k10_sound_start(int channel, const uint8_t *data, uint32_t samples,
                     uint32_t srcRate, int volL, int volR);

// Updates the gains of an already-playing voice.
void k10_sound_set_params(int channel, int volL, int volR);

// Silences a voice immediately.
void k10_sound_stop(int channel);

// Nonzero if the given voice has not yet reached the end of its data.
int k10_sound_is_playing(int channel);

#ifdef __cplusplus
}
#endif
