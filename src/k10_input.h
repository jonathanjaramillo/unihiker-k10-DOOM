#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void k10_input_begin(void);
// doomgeneric's DG_GetKey contract: returns 1 and fills *pressed / *key when an
// event is available, 0 when the queue is empty.
int  k10_input_poll(int *pressed, unsigned char *key);

// BOOT (GPIO0) doubles as the I2S bit clock once sound is running; the sound
// module calls this to stop polling it, since digitalRead would otherwise see
// the I2S bitstream and read as a button being mashed.
void k10_input_set_boot_enabled(int enabled);

#ifdef __cplusplus
}
#endif
