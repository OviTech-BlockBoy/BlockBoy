# BlockBoy - GB/GBC Edition

Game Boy and Game Boy Color emulator firmware for the BlockBoy handheld (ESP32-S3 N4R2).

Based on [Retro-Go](https://github.com/ducalex/retro-go) by ducalex.

## Supported systems
- Nintendo Game Boy
- Nintendo Game Boy Color

## Features
- In-game menu with save states
- Multiple save slots per game
- GB color palettes, RTC adjust and save
- Favorites and recently played
- Cover art and save state previews
- Scaling and filtering options
- Turbo speed / fast forward
- Customizable launcher with themes
- WiFi file manager
- ZIP file support

## Hardware
- ESP32-S3 N4R2 (4MB Flash, 2MB PSRAM)
- ILI9341 320x240 display
- SD card for ROM storage
- External I2S DAC audio

## Installation
1. Build the firmware (see [Building](#building))
2. Flash the image: `esptool.py write_flash --flash_size detect 0x0 blockboy_*.img`

## Building
Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) v5.x.

```bash
python rg_tool.py build-img all --target 0v1Tech-BlockBoy-N4R2
```

## ROM files
Place your ROM files on the SD card:
- Game Boy: `/retro-go/roms/gb/`
- Game Boy Color: `/retro-go/roms/gbc/`

Supported extensions: `.gb`, `.gbc`, `.zip`

## BIOS files (optional)
- GB: `/retro-go/bios/gb_bios.bin`
- GBC: `/retro-go/bios/gbc_bios.bin`

## Cover art
Game covers should be placed in `/romart/` on your SD card. PNG format, 160x168, 8bit.

## Changes from upstream Retro-Go
This is a modified version of [Retro-Go](https://github.com/ducalex/retro-go) by ducalex, with the following changes:
- Added custom 0v1Tech-BlockBoy-N4R2 target (ESP32-S3, 4MB flash, 2MB quad PSRAM)
- Added USB Mass Storage mode (access SD card via USB-C)
- Added showcase mode feature (cycle through GB/GBC games automatically)
- Added boot animation options (blocks, scroll, off)
- Stripped down to Game Boy and Game Boy Color emulation only
- Removed NES, SNES, SMS, Game Gear, PC Engine, Lynx, Game & Watch, Genesis, and DOOM emulators
- Removed all unused targets (only 0v1Tech-BlockBoy-N4R2 remains)
- Customized launcher to show only GB/GBC tabs
- Removed unused launcher images to reduce binary size

## License
This project is licensed under the [GPLv2](COPYING), the same license as the original Retro-Go project.

## Acknowledgements
- [Retro-Go](https://github.com/ducalex/retro-go) by ducalex
- gnuboy Game Boy/Color emulator
- [lodepng](https://github.com/lvandeve/lodepng/) for PNG support
