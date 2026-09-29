# DOOM on UNIHIKER K10

A PlatformIO / Arduino port of doomgeneric for the UNIHIKER K10, with a
320 × 200 display backend, SD card WAD loading, input handling, and audio.

Clone with the vendor framework and documentation references:

```sh
git clone --recurse-submodules https://github.com/jonathanjaramillo/unihiker-k10-DOOM.git
cd unihiker-k10-DOOM
```

Install PlatformIO, place `doom1.wad` in the root of a FAT32 SD card, and
insert the card into the K10. The WAD is supplied separately.
Update the upload and monitor ports in `platformio.ini` for your computer.

```sh
pio run -e k10
pio run -e k10 -t upload
pio device monitor -b 115200
```

The `sdprobe` environment provides an SD card diagnostic build.
See [HARDWARE-NOTES.md](HARDWARE-NOTES.md) for board details and SD card findings.
The locally collected K10 schematic is in `docs/UnihikerK10Schematic.pdf`.

The `game_controller` folder is excluded while its implementation is being
replaced. The current firmware input code is in `src/k10_input.cpp`.

The vendored doomgeneric source and its license are in `lib/doomgeneric`.
Vendor framework and documentation are pinned as Git submodules.
