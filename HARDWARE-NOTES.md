# UNIHIKER K10 — hardware notes

Everything below was verified on the board or read out of the vendor sources /
schematic, not assumed. Sources are named so each claim can be rechecked.

## SoC / memory

- ESP32-S3-WROOM-1, 16 MB flash, **8 MB PSRAM** (quad, 80 MHz).
  Measured: `ESP.getPsramSize()` = 8,386,295; largest free block 7.87 MB;
  a 4 MB `heap_caps_malloc(MALLOC_CAP_SPIRAM)` succeeds.
- `CONFIG_SPIRAM_USE_MALLOC=y` with `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`,
  so **any `malloc` over 4 KB transparently lands in PSRAM**. No `EXT_RAM_ATTR`
  is available (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` is absent), so
  oversized *static* arrays must be converted to runtime allocations instead.
- Toolchain: PlatformIO `unihiker` platform, Arduino core 2.0.17 on
  **ESP-IDF 4.4.4**, precompiled. `CONFIG_LOG_MAXIMUM_LEVEL=1`, so most IDF
  WARN/INFO logging is compiled out of the shipped `.a` files.

## Display

- ILI9341, 240x320 portrait (use `setRotation(1)` for 320x240 landscape).
- **Write-only** — the panel's SDO is not routed to the module (schematic p.5).
- `TFT_eSPI` ships preconfigured: MOSI 21, SCLK 12, CS 14, DC 13, 40 MHz
  (`libraries/TFT_eSPI/User_Setup.h:171-176,364`).
- `SUPPORT_TRANSACTIONS` is commented out in `User_Setup.h` but force-enabled
  for ESP32 at `Processors/TFT_eSPI_ESP32.h:37-39`.
- Backlight is **not** a GPIO — it is `eLCD_BLK` on the I2C expander.

## SD card — a separate SPI bus from the LCD

This contradicts a natural reading of the schematic; it was settled by
disassembling the vendor's precompiled `libSD.a`:

```
fs::SDFS::begin  ->  SPIClass::begin(sck=44, miso=41, mosi=42, ss=-1)
```

- **SCK 44, MISO 41, MOSI 42, CS 40**, on Arduino's `SPI1` (= HSPI = `SPI3_HOST`).
- CS is **active low** (confirmed by raw CMD0: card replies `0x01`, CMD8 replies
  `0x01` with check pattern `AA`).
- GPIO 40 is shared with the GT30L24A3W font chip (`GT30L24A3W.h:19` defines
  `FONTCS 40`); the two devices' selects are inverted relative to each other by
  an MMBT3904 (schematic p.9). Driving 40 low selects the card.
- Note `pins_arduino.h` advertises `SCK=12, MOSI=21` — those are the **LCD**
  pins, not the card's. The LCD and SD genuinely do not share a bus, so display
  blits and WAD reads do not contend.

## Buttons

- **Button A and Button B are on an XL9535 I2C IO expander at address 0x20**,
  active low with 10K pullups (schematic p.3/p.4). Read them with the vendor
  API: `digital_read(eP5_KeyA)` / `digital_read(eP11_KeyB)` from `initBoard.h`
  (linked by default via `-lmodules`).
- **Trap:** `pins_arduino.h` says `P5 = 12 //key A`. That is the *edge
  connector* label. GPIO 12 is SPI SCLK. Never `digitalRead(12)` for button A.
- **BOOT button is a plain GPIO 0** and works with `digitalRead` — a free third
  button.
- Also on the expander: `eLCD_BLK` (backlight), `eCamera_rst`, `eAmp_Gain`.

## Audio (unused so far)

- Output: NS4168 I2S class-D amp. Input: ES7243E ADC + 2 MEMS mics.
- `init_board()` logs `Es7243e initialize failed!` on every boot. That is the
  **microphone** codec and is unrelated to SD, display or buttons.

## Other I2C devices

AHT20 @ 0x38 (temp/humidity), LTR-303ALS @ 0x29 (light), SC7A20H @ 0x19
(accelerometer), XL9535 @ 0x20 (IO expander).

## The SD card that shipped with this unit is malformed

`SD.begin()` and `esp_vfs_fat_sdspi_mount()` both fail with `FR_NO_FILESYSTEM`
even though the card initialises fine and sector 0 / the VBR read back
correctly. The volume is genuinely non-conformant, in two ways:

1. `BS_FilSysType32` (offset 82) is `"FAT32  \0"` — two spaces and a NUL —
   where the spec requires `"FAT32   "`. FatFs `memcmp`s all 8 bytes.
2. More seriously, **the FAT is one sector too small for the cluster count the
   BPB declares**:

   | field | value |
   |---|---|
   | `BytsPerSec` | 512 |
   | `SecPerClus` | 16 |
   | `RsvdSecCnt` | 2988 |
   | `NumFATs` | 2 |
   | `FATSz32` | 14890 |
   | `TotSec32` | 30527488 |

   system area = 2988 + 2x14890 = 32768 sectors
   clusters    = (30527488 - 32768) / 16 = 1905920
   FAT entries = 1905922 x 4 B = 7623688 B -> **14891** sectors needed
   declared    = **14890**

   FatFs's check `if (fs->fsize < (szbfat + SS-1)/SS) return FR_NO_FILESYSTEM;`
   therefore fails. macOS and Windows tolerate this; FatFs does not.

The MBR itself is fine (one partition, type 0x0C FAT32-LBA, LBA 8192,
30527488 sectors) and the OEM string "Vinpower Digital" indicates a duplicator
-written factory image rather than a normal format.

**Fix: reformat the card as FAT32 with `newfs_msdos` / Disk Utility**, which
computes these fields correctly, then re-copy the WAD.

## The WAD

`doom1.wad`, shareware v1.9: 4,196,020 bytes, magic `IWAD`,
md5 `f0cefca49926d00903cf57551d901abe` — the canonical DOOM1.WAD. Episode 1 only.
