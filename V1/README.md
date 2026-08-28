# BlockBoy V1

Firmware for the BlockBoy V1 handheld (ESP32-S3 N4R2). With 4 MB of flash it runs
Game Boy, Game Boy Color and NES; the larger models add seven more systems.

Based on [Retro-Go](https://github.com/ducalex/retro-go) by ducalex.

See [MODELS.md](MODELS.md) for the differences between BlockBoy V1, V2 and V3.

## Supported systems
- Nintendo Game Boy
- Nintendo Game Boy Color
- Nintendo Entertainment System (NES)

## Features
- Wireless controllers over Bluetooth, with a button-remap wizard and a test screen
- Game Link: wireless Game Boy / Game Boy Color link cable between two BlockBoys
- Wi-Fi with a network manager (five saved networks) and automatic clock sync
- Web file manager: browse, upload and delete SD card files from your browser,
  password protected, reachable at `blockboy.local`
- Over-the-air firmware updates from the menu, verified per file before writing
- USB-C mass storage: the SD card shows up on your PC without opening the case
- Audio over USB-C: plug in a USB-C headphone adapter (it needs its own USB power)
- Showcase mode with game selection, sleep timer and scheduled wake-up
- In-game menu with save states and multiple slots per game
- Favorites, recently played, cover art and save state previews
- Scaling and filtering
- Turbo speed / fast forward
- Customisable launcher with built-in and SD card themes
- Low-battery warning
- ZIP file support

## Hardware
- ESP32-S3 N4R2 (4MB Flash, 2MB PSRAM)
- ILI9341 320x240 display
- SD card for ROM storage
- External I2S DAC audio

## Updating
Update over Wi-Fi from the device itself: **Options > Firmware update**.

Coming from firmware 1.0? That release does not have this update system, so it
takes one pass with the [web flasher](https://blockboy.nl/pages/firmware) over
USB. Every version after that arrives over Wi-Fi.

The web flasher also stays available for recovery.

## Installation
1. Build the firmware (see [Building](#building))
2. Flash the image: `esptool.py write_flash --flash_size detect 0x0 blockboy_*.img`

## Building
Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) v5.x (built and tested with v5.5).

```bash
python rg_tool.py build-img all --target 0v1Tech-BlockBoy-N4R2
```

## ROM files
Place your ROM files on the SD card:
- Game Boy: `/roms/gb/`
- Game Boy Color: `/roms/gbc/`
- NES: `/roms/nes/`

Supported extensions: `.gb`, `.gbc`, `.nes`, `.fc`, `.fds`, `.nsf`, `.zip`

## BIOS files (optional)
- GB: `/BlockBoy/bios/gb_bios.bin`
- GBC: `/BlockBoy/bios/gbc_bios.bin`
- Famicom Disk System: `/BlockBoy/bios/fds_bios.bin` (only needed for `.fds` games)

## Cover art
Game covers should be placed in `/romart/` on your SD card. PNG format, 160x168, 8bit.

## Changes from upstream Retro-Go
This is a modified version of [Retro-Go](https://github.com/ducalex/retro-go) by
ducalex, with the following changes:
- Added the 0v1Tech-BlockBoy-N4R2 target (ESP32-S3, 4MB flash, 2MB quad PSRAM)
- Own over-the-air update system: per-model manifests, a partition-layout check
  and a SHA-256 per file, replacing the upstream updater that pointed at
  ducalex's own releases
- Bluetooth controller support with a button-remap wizard and a test screen
- Game Link: the upstream netplay code extended into a wireless Game Boy /
  Game Boy Color link cable between two BlockBoys
- Wi-Fi network manager: five saved networks, scanning, and a low-power mode
  that only connects to sync the clock
- Web file manager behind a password that differs per device, reachable at
  `blockboy.local`
- USB Mass Storage mode (SD card over USB-C)
- Audio over USB-C: plug in a USB-C headphone adapter (it needs its own USB power)
- Showcase mode with game selection, sleep timer and scheduled wake-up
- Boot animation options (blocks, scroll, off)
- Battery warning threshold and calibration
- Limited to Game Boy, Game Boy Color and NES: 4MB of flash leaves no room for
  the other systems
- Removed the SNES and MSX emulators (non-commercial Snes9x and fMSX licenses)
- Removed the PC Engine and Atari Lynx emulators
- Removed all unused targets and launcher images to save flash

## License
This project is licensed under the [GPLv2](COPYING), the same license as the original Retro-Go project.

## Acknowledgements
- [Retro-Go](https://github.com/ducalex/retro-go) by ducalex
- gnuboy Game Boy/Color emulator
- [lodepng](https://github.com/lvandeve/lodepng/) for PNG support
