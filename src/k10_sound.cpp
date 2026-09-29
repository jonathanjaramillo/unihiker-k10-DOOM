// PCM voice mixer over I2S -- see k10_sound.h for why this file must not
// include any Doom headers.
//
// I2S's bit clock (BCLK) lives on GPIO0, which doubles as the BOOT button, so
// once this module owns the I2S peripheral the BOOT button stops working
// (k10_input_set_boot_enabled(0)); see HARDWARE-NOTES.md.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "driver/i2s.h"

extern "C" {
#include "initBoard.h"
}
#include "k10_input.h"
#include "k10_sound.h"

#define I2S_PORT       I2S_NUM_0
#define OUT_RATE       22050
#define DMA_BUF_LEN    256     // frames per DMA buffer
#define DMA_BUF_COUNT  4
#define MAX_VOICES     16      // >= snd_channels (default 8), room to spare

// I2S pins (unihiker_k10.h). BCLK is GPIO 0 -- see file header.
#define IIS_BLCK 0
#define IIS_LRCK 38
#define IIS_DOUT 45
#define IIS_MCLK 3

typedef struct {
  const uint8_t *data;    // unsigned 8-bit PCM, owned by the caller (WAD cache)
  uint32_t       length;  // in samples
  uint32_t       pos;     // 16.16 fixed-point read position
  uint32_t       step;    // 16.16 fixed-point resample step (srcRate/OUT_RATE)
  int            volL, volR;
  bool           active;
} Voice;

static Voice s_voices[MAX_VOICES];
static SemaphoreHandle_t s_lock = nullptr;
static bool s_running = false;

static inline void lock()   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock() { xSemaphoreGive(s_lock); }

static void mixerTask(void *)
{
  static int16_t buf[DMA_BUF_LEN * 2];   // stereo frames

  for (;;) {
    lock();
    for (int i = 0; i < DMA_BUF_LEN; i++) {
      int32_t accL = 0, accR = 0;

      for (int c = 0; c < MAX_VOICES; c++) {
        Voice *v = &s_voices[c];
        if (!v->active) continue;

        uint32_t idx = v->pos >> 16;
        if (idx >= v->length) { v->active = false; continue; }

        int sample = (int)v->data[idx] - 128;   // unsigned -> signed
        accL += sample * v->volL;
        accR += sample * v->volR;
        v->pos += v->step;
      }

      // A single full-volume voice peaks at 127*127 (~half of int16 range),
      // leaving headroom for a handful of simultaneous sounds before clamping.
      buf[i * 2 + 0] = (int16_t)constrain(accL, -32768, 32767);
      buf[i * 2 + 1] = (int16_t)constrain(accR, -32768, 32767);
    }
    unlock();

    size_t written = 0;
    i2s_write(I2S_PORT, buf, sizeof(buf), &written, portMAX_DELAY);
  }
}

extern "C" int k10_sound_init(void)
{
  if (s_running) return 1;

  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) return 0;
  memset(s_voices, 0, sizeof(s_voices));

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = OUT_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.dma_buf_count = DMA_BUF_COUNT;
  cfg.dma_buf_len = DMA_BUF_LEN;
  cfg.tx_desc_auto_clear = true;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = IIS_MCLK;
  pins.bck_io_num = IIS_BLCK;
  pins.ws_io_num = IIS_LRCK;
  pins.data_out_num = IIS_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  esp_err_t a = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  esp_err_t b = i2s_set_pin(I2S_PORT, &pins);
  Serial.printf("[snd] i2s install=%s set_pin=%s\n", esp_err_to_name(a), esp_err_to_name(b));
  if (a != ESP_OK || b != ESP_OK) return 0;

  // BCLK shares GPIO0 with the BOOT button; stop polling it now that I2S
  // owns the pin, or the bitstream toggling gets misread as button mashing.
  k10_input_set_boot_enabled(0);

  // digital_write() on the XL9535 expander is not bit-safe and can clobber
  // eLCD_BLK, so re-assert the backlight immediately after enabling the amp.
  digital_write(eAmp_Gain, 1);
  digital_write(eLCD_BLK, 1);

  BaseType_t ok = xTaskCreatePinnedToCore(mixerTask, "k10_snd", 4096, nullptr, 4, nullptr, 0);
  if (ok != pdPASS) return 0;

  s_running = true;
  return 1;
}

extern bool k10_input_debug;   // toggle at runtime with "=", see k10_input.cpp

extern "C" int k10_sound_start(int channel, const uint8_t *data, uint32_t samples,
                                uint32_t srcRate, int volL, int volR)
{
  if (!s_running || channel < 0 || channel >= MAX_VOICES || samples == 0) return -1;
  if (srcRate == 0) srcRate = 11025;

  if (k10_input_debug)
    Serial.printf("[snd] ch%d: %u samples @ %u Hz, vol L%d R%d\n",
                  channel, samples, srcRate, volL, volR);

  lock();
  Voice *v = &s_voices[channel];
  v->data   = data;
  v->length = samples;
  v->pos    = 0;
  v->step   = (uint32_t)(((uint64_t)srcRate << 16) / OUT_RATE);
  v->volL   = volL;
  v->volR   = volR;
  v->active = true;
  unlock();

  return channel;
}

extern "C" void k10_sound_set_params(int channel, int volL, int volR)
{
  if (channel < 0 || channel >= MAX_VOICES) return;
  lock();
  s_voices[channel].volL = volL;
  s_voices[channel].volR = volR;
  unlock();
}

extern "C" void k10_sound_stop(int channel)
{
  if (channel < 0 || channel >= MAX_VOICES) return;
  lock();
  s_voices[channel].active = false;
  unlock();
}

extern "C" int k10_sound_is_playing(int channel)
{
  if (channel < 0 || channel >= MAX_VOICES) return 0;
  lock();
  int playing = s_voices[channel].active ? 1 : 0;
  unlock();
  return playing;
}
