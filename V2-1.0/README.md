# BlockBoy V2

Multi-system retro game emulator firmware for the BlockBoy V2 handheld (ESP32-S3 N16R8).

Based on [Retro-Go](https://github.com/ducalex/retro-go) by ducalex.

## Supported systems
- Nintendo: NES, Game Boy, Game Boy Color, Game & Watch
- Sega: Master System, Game Gear, Mega Drive / Genesis
- Coleco: ColecoVision
- Others: DOOM (including mods!)

## Features
- In-game menu with save states
- Multiple save slots per game
- GB color palettes, RTC adjust and save
- NES color palettes, PAL roms, NSF support
- Favorites and recently played
- Cover art and save state previews
- Scaling and filtering options
- Turbo speed / fast forward
- Customizable launcher with themes
- WiFi file manager
- ZIP file support

## Hardware
- ESP32-S3 N16R8 (16MB Flash, 8MB PSRAM)
- Display (configurable in target)
- SD card for ROM storage

## Installation
1. Build the firmware (see [Building](#building))
2. Flash: `esptool.py write_flash --flash_size detect 0x0 blockboy_*.img`

## Building
Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) v4.4 - v5.x.

```bash
python rg_tool.py build-img all --target 0v1Tech-BlockBoy-N16R8
```

Clean build:
```bash
python rg_tool.py clean all --target 0v1Tech-BlockBoy-N16R8
python rg_tool.py build-img all --target 0v1Tech-BlockBoy-N16R8
```

## Flashing
```bash
python rg_tool.py --target 0v1Tech-BlockBoy-N16R8 --port COM3 install
```

Or manually:
```bash
esptool.py write_flash --flash_size detect 0x0 blockboy_*.img
```

## ROM files
Place ROMs on SD card in `/retro-go/roms/<system>/`:
- NES: `.nes`, `.fc`, `.fds`, `.nsf`, `.zip`
- Game Boy: `.gb`, `.gbc`, `.zip`
- Game Boy Color: `.gbc`, `.gb`, `.zip`
- Game & Watch: `.gw`
- Master System: `.sms`, `.sg`, `.zip`
- Game Gear: `.gg`, `.zip`
- Mega Drive: `.md`, `.gen`, `.bin`, `.zip`
- ColecoVision: `.col`, `.rom`, `.zip`
- DOOM: `.wad`, `.zip`

## BIOS files (optional)
- GB: `/retro-go/bios/gb_bios.bin`
- GBC: `/retro-go/bios/gbc_bios.bin`
- FDS: `/retro-go/bios/fds_bios.bin`

## Cover art
Game covers in `/romart/` on SD card. PNG format, 160x168, 8bit.

## Changes from upstream Retro-Go
This is a modified version of [Retro-Go](https://github.com/ducalex/retro-go) by ducalex:
- Added custom 0v1Tech-BlockBoy-N16R8 target (ESP32-S3, 16MB flash, 8MB octal PSRAM)
- Added USB Mass Storage mode (access SD card via USB-C)
- Added showcase mode feature (cycle through games automatically)
- Added boot animation options (blocks, scroll, off)
- Removed SNES emulator (non-commercial Snes9x license)
- Removed MSX emulator (non-commercial fMSX license)
- Removed PC Engine emulator (not needed)
- Removed Atari Lynx emulator (not needed)
- Removed unused targets (only 0v1Tech-BlockBoy-N16R8 remains)

## License
This project is licensed under the [GPLv2](COPYING), the same license as the original Retro-Go project.

## Acknowledgements
- [Retro-Go](https://github.com/ducalex/retro-go) by ducalex
- gnuboy Game Boy/Color emulator
- [nofrendo](https://github.com/ducalex/retro-go) NES emulator
- [PrBoom](http://prboom.sourceforge.net/) DOOM engine
- [Gwenesis](https://github.com/bzhxx/gwenesis/) Genesis emulator by bzhxx
- [lcd-game-emulator](https://github.com/bzhxx/lcd-game-emulator) Game & Watch emulator by bzhxx
- [lodepng](https://github.com/lvandeve/lodepng/) for PNG support
