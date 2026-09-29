// UNIHIKER K10 backend for doomgeneric.
//
// Doom renders 320x200; the panel is 320x240 in landscape. We scale vertically
// by replicating every 5th scanline (200 * 6/5 = 240), which both fills the
// screen and restores Doom's intended 4:3 aspect -- the 320x200 source was
// authored for non-square pixels.
//
// The framebuffer is already RGB565: doomgeneric is started with
// "-gfxmode rgb565", so cmap_to_fb() packs 565 straight into DG_ScreenBuffer.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

extern "C" {
#include "initBoard.h"
#include "doomgeneric.h"
#include "doomkeys.h"
// Doom's 8-bit render target (internal SRAM) and the RGB565 palette that
// i_video.c keeps in step with it.
extern unsigned char  *I_VideoBuffer;
extern unsigned short  DG_Palette565[256];
}

#include "k10_input.h"

#define SCREEN_W  320
#define SCREEN_H  240
#define DOOM_W    DOOMGENERIC_RESX   // 320
#define DOOM_H    DOOMGENERIC_RESY   // 200

// Push the frame in horizontal bands. This deliberately uses BLOCKING
// pushPixels rather than pushPixelsDMA: on this panel the DMA path delivered
// only the first frame and then stopped (verified with a standalone animation
// test), which is what produced a near-static black screen. DMA was also
// buying nothing measurable, since pushPixelsDMA waits on the previous
// transfer anyway.
#define BAND_LINES 24
static uint16_t *s_band = nullptr;

static TFT_eSPI tft;

// --- frame timing -----------------------------------------------------------
static uint32_t s_frames = 0;
static uint32_t s_fpsWindowStart = 0;
static uint32_t s_blitUs = 0;       // time inside DG_DrawFrame
static uint32_t s_lastFrameEnd = 0; // to derive engine time

extern "C" void DG_Init()
{
  // Claim Doom's render target from internal SRAM before ANYTHING else in this
  // function allocates -- tft.initDMA() and the band buffers fragment the
  // internal heap enough that a contiguous 64KB is no longer available, and
  // I_InitGraphics() runs far too late to win the race against the engine's
  // own internal allocations.
  Serial.printf("DG_Init: internal heap %u free, largest block %u\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  if (I_VideoBuffer == nullptr) {
    I_VideoBuffer = (unsigned char *)heap_caps_malloc(DOOM_W * DOOM_H,
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("DG_Init: video buffer %p (%s)\n", I_VideoBuffer,
                  I_VideoBuffer ? "internal SRAM" : "FAILED -> PSRAM fallback");
  }

  digital_write(eLCD_BLK, 1);          // backlight on, via the XL9535 expander
  tft.begin();
  tft.setRotation(1);                  // landscape: 320x240
  tft.setSwapBytes(true);              // ILI9341 wants big-endian 565
  tft.fillScreen(TFT_BLACK);

  s_band = (uint16_t *)heap_caps_malloc(SCREEN_W * BAND_LINES * sizeof(uint16_t),
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_band) Serial.println(F("DG_Init: band buffer alloc FAILED"));
  s_fpsWindowStart = millis();
  Serial.println(F("DG_Init: display ready"));
}

extern "C" void DG_DrawFrame()
{
  const uint8_t *src = I_VideoBuffer;
  if (!src || !s_band) return;
  uint32_t blitStart = micros();

  const uint16_t *pal = DG_Palette565;

  tft.startWrite();
  tft.setAddrWindow(0, 0, SCREEN_W, SCREEN_H);

  int slot = 0;
  for (int outY = 0; outY < SCREEN_H; outY++) {
    // 240 output rows from 200 source rows: every 5th row is emitted twice,
    // which fills the panel and restores Doom's intended 4:3 aspect.
    const int srcY = (outY * DOOM_H) / SCREEN_H;
    const uint8_t *srcLine = src + (size_t)srcY * DOOM_W;
    uint16_t *dst = s_band + (size_t)slot * SCREEN_W;

    // Palette lookup straight into the band buffer; both are internal SRAM.
    for (int x = 0; x < SCREEN_W; x++) dst[x] = pal[srcLine[x]];

    if (++slot == BAND_LINES || outY == SCREEN_H - 1) {
      tft.pushPixels(s_band, SCREEN_W * slot);
      slot = 0;
    }
  }

  tft.endWrite();
  s_blitUs += micros() - blitStart;

  s_frames++;
  uint32_t now = millis();
  if (now - s_fpsWindowStart >= 5000) {
    uint32_t elapsed = now - s_fpsWindowStart;
    float blitMs = (s_blitUs / 1000.0f) / s_frames;
    float totalMs = (float)elapsed / s_frames;
    Serial.printf("[perf] %.1f fps | frame %.1f ms = engine %.1f + blit %.1f | heap %u int / %u psram\n",
                  s_frames * 1000.0f / elapsed, totalMs, totalMs - blitMs, blitMs,
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    s_frames = 0;
    s_blitUs = 0;
    s_fpsWindowStart = now;
  }
}

extern "C" void DG_SleepMs(uint32_t ms)
{
  vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

extern "C" uint32_t DG_GetTicksMs()
{
  return millis();
}

extern "C" int DG_GetKey(int *pressed, unsigned char *key)
{
  return k10_input_poll(pressed, key);
}

extern "C" void DG_SetWindowTitle(const char *title)
{
  (void)title;
}
