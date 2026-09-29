// Doom on the UNIHIKER K10.
//
// setup() brings up the board, verifies the things that silently kill the port
// (PSRAM, the WAD on SD), then hands off to doomgeneric on its own task.

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_heap_caps.h>

extern "C" {
#include "initBoard.h"
#include "doomgeneric.h"
}
#include "k10_input.h"

static const char *WAD_PATH = "/sd/doom1.wad";

static bool mountSd()
{
  // Note: the vendor's libSD.a defaults to SPI1 with the K10's own SD pins
  // (SCK 44, MISO 41, MOSI 42, CS 40) -- a different bus from the LCD.
  const uint32_t freqs[] = {20000000, 10000000, 4000000, 1000000, 400000};
  for (uint32_t f : freqs) {
    if (SD.begin(SS, SPI1, f)) {
      Serial.printf("SD mounted at %u Hz, %llu MB\n", f, SD.cardSize() / (1024ULL * 1024ULL));
      return true;
    }
    SD.end();
    delay(50);
  }
  return false;
}

static bool checkWad()
{
  File wad = SD.open("/doom1.wad");
  if (!wad) {
    Serial.println(F("FATAL: /doom1.wad not found on the SD card."));
    File root = SD.open("/");
    Serial.println(F("Root listing:"));
    for (File f = root.openNextFile(); f; f = root.openNextFile())
      Serial.printf("  %-24s %10u%s\n", f.name(), (unsigned)f.size(), f.isDirectory() ? "  <dir>" : "");
    return false;
  }
  char magic[5] = {0};
  wad.readBytes(magic, 4);
  size_t sz = wad.size();
  wad.close();
  Serial.printf("doom1.wad: magic '%s', %u bytes\n", magic, (unsigned)sz);
  if (strcmp(magic, "IWAD") != 0) {
    Serial.println(F("FATAL: not an IWAD (was the .deb copied instead of the WAD?)"));
    return false;
  }
  return true;
}

static void doomTask(void *)
{
  // -gfxmode rgb565 makes cmap_to_fb() pack RGB565 straight into
  // DG_ScreenBuffer, halving per-frame memory traffic vs the 32bpp default.
  // -mb 4 sizes the zone heap; CONFIG_SPIRAM_USE_MALLOC routes it to PSRAM.
  static char *argv[] = {
    (char *)"doom",
    (char *)"-iwad",    (char *)WAD_PATH,
    (char *)"-gfxmode", (char *)"rgb565",
    (char *)"-mb",      (char *)"4",
  };
  const int argc = sizeof(argv) / sizeof(argv[0]);

  Serial.println(F("\n=== starting doomgeneric ==="));
  doomgeneric_Create(argc, argv);   // initialises and renders one frame

  // D_DoomLoop() does its one-time setup, ticks once and returns, so the
  // frame loop belongs to us.
  //
  // The yield is not optional: this task is priority 5 pinned to core 1 and
  // doomgeneric_Tick() never blocks, so without it core 1's idle task is
  // starved and the task watchdog reboots the board after ~5 s. At ~100 ms a
  // frame, one tick costs nothing.
  for (;;) {
    doomgeneric_Tick();
    vTaskDelay(1);
  }
}

void setup()
{
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  delay(300);

  Serial.println(F("\n\n=== UNIHIKER K10 DOOM ==="));
  Serial.printf("%s rev%d, %d MHz, flash %u MB\n", ESP.getChipModel(), ESP.getChipRevision(),
                getCpuFrequencyMhz(), ESP.getFlashChipSize() / (1024 * 1024));

  init_board();     // XL9535 expander + board power rails

  // NOTE: digital_write() on the XL9535 is NOT bit-safe -- writing eAmp_Gain
  // was observed to clobber eLCD_BLK and blank the display. Any amp write must
  // re-assert the backlight afterwards. The buzz source is still unidentified
  // (backlight, display SPI, SD traffic and the amp itself have all been ruled
  // out on hardware), so this no longer blindly pokes the amp; use the runtime
  // debug commands instead.

  size_t psram = ESP.getPsramSize();
  Serial.printf("PSRAM %.2f MB (largest block %.2f MB)\n", psram / 1048576.0,
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1048576.0);
  if (psram < 5 * 1024 * 1024) {
    Serial.println(F("WARNING: under 5MB PSRAM. Lower the -mb zone size in doomTask()."));
  }

  if (!mountSd()) {
    Serial.println(F("FATAL: SD card did not mount at any clock rate."));
    Serial.println(F("  Is the card seated? (push until it clicks)"));
    Serial.println(F("  Is it FAT32? FatFs cannot read exFAT."));
    return;
  }
  if (!checkWad()) return;

  // Self-test the XL9535 read path without needing a button press: drive an
  // unused output pin both ways and read it back. If these track, digital_read()
  // works and a silent button is genuinely an unpressed button.
  {
    digital_write(eCamera_rst, 0); delay(5);
    uint8_t lo = digital_read(eCamera_rst);
    digital_write(eCamera_rst, 1); delay(5);
    uint8_t hi = digital_read(eCamera_rst);
    Serial.printf("[xl9535] readback low=%u high=%u -> %s\n", lo, hi,
                  (lo == 0 && hi == 1) ? "READ PATH OK" : "READ PATH SUSPECT");
    Serial.printf("[xl9535] buttons at boot: A=%u B=%u BOOT=%u (1 = released)\n",
                  digital_read(eP5_KeyA), digital_read(eP11_KeyB), digitalRead(0));
  }

  k10_input_begin();

  // Doom needs a far bigger stack than the Arduino loop task provides.
  xTaskCreatePinnedToCore(doomTask, "doom", 32768, nullptr, 5, nullptr, 1);
}

void loop()
{
  vTaskDelay(pdMS_TO_TICKS(1000));
}
