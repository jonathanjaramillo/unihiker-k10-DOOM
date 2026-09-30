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

The optional I2C game controller is auto-detected at address `0x5A` on the
K10's shared I2C bus (SDA GPIO47, SCL GPIO48). It can be connected or removed
while Doom is running; unplugging it releases its held keys. Set
`-DK10_GAME_CONTROLLER=0` in `platformio.ini` to compile without its support.
The driver lives in `lib/GameController`; Doom mappings are in `src/k10_input.cpp`.

| Controller | Doom action |
| --- | --- |
| Joystick up/down | Move forward/back; navigate menus |
| Joystick left/right | Turn left/right; navigate menus |
| A | Menu/back (Escape) |
| B | Use/open doors |
| C | Fire |
| D | Automap (Tab) |
| OK | Menu select (Enter) |

Serial keyboard controls remain available: WASD or arrow keys for movement,
`,`/`.` for strafing, F to fire, Space to use, Enter to select, Escape for the
menu, Tab for the automap, and 1–9 for weapons. Onboard A fires and B uses;
BOOT toggles the automap until audio takes over GPIO0. Send `/` over serial
for button/controller status, or `=` to toggle key-event logging.

The vendored doomgeneric source and its license are in `lib/doomgeneric`.
Vendor framework and documentation are pinned as Git submodules.
