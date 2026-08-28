# BlockBoy V2

Multi-system retro game emulator firmware for the BlockBoy V2 handheld (ESP32-S3 N16R8).

Based on [Retro-Go](https://github.com/ducalex/retro-go) by ducalex.

See [MODELS.md](MODELS.md) for the differences between BlockBoy V1, V2 and V3.

## Supported systems
- Nintendo: NES, Game Boy, Game Boy Color, Game Boy Advance, Game & Watch
- Sega: Master System, Game Gear, Mega Drive / Genesis
- Coleco: ColecoVision
- Others: DOOM (including mods)

## Features
- Game Boy Advance, tuned for the Pokemon titles (JIT recompiler, dual-core rendering)
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
- ESP32-S3 N16R8 (16MB Flash, 8MB PSRAM)
- Display (configurable in target)
- SD card for ROM storage

## Updating
Update over Wi-Fi from the device itself: **Options > Firmware update**.

Coming from firmware 1.0? That release does not have this update system, so it
takes one pass with the [web flasher](https://blockboy.nl/pages/firmware) over
USB. Every version after that arrives over Wi-Fi.

The web flasher also stays available for recovery.

## Installation
1. Build the firmware (see [Building](#building))
2. Flash: `esptool.py write_flash --flash_size detect 0x0 blockboy_*.img`

## Building
Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) v5.x (built and tested with v5.5).

```bash
python rg_tool.py build-img all --target 0v1Tech-BlockBoy-N16R8
```

Clean build:
```bash
python rg_tool.py clean all --target 0v1Tech-BlockBoy-N16R8
python rg_tool.py build-img all --target 0v1Tech-BlockBoy-N16R8
```

## Flashing
Replace `COMx` with the port your BlockBoy shows up on: `COM7` and the like on
Windows (Device Manager > Ports), `/dev/ttyACM0` or `/dev/ttyUSB0` on Linux,
`/dev/cu.usbmodem*` on macOS.

```bash
python rg_tool.py --target 0v1Tech-BlockBoy-N16R8 --port COMx install
```

Or manually:
```bash
esptool.py write_flash --flash_size detect 0x0 blockboy_*.img
```

## ROM files
Place ROMs on SD card in `/roms/<system>/`:
- NES: `.nes`, `.fc`, `.fds`, `.nsf`, `.zip`
- Game Boy: `.gb`, `.gbc`, `.zip`
- Game Boy Color: `.gbc`, `.gb`, `.zip`
- Game Boy Advance: `.gba`, `.zip`
- Game & Watch: `.gw`
- Master System: `.sms`, `.sg`, `.zip`
- Game Gear: `.gg`, `.zip`
- Mega Drive: `.md`, `.gen`, `.bin`, `.zip`
- ColecoVision: `.col`, `.rom`, `.zip`
- DOOM: `.wad`, `.zip`

## BIOS files (optional)
- GB: `/BlockBoy/bios/gb_bios.bin`
- GBC: `/BlockBoy/bios/gbc_bios.bin`
- FDS: `/BlockBoy/bios/fds_bios.bin`
- GBA: `/BlockBoy/bios/gba_bios.bin` (the built-in open-source BIOS is used otherwise)

## Cover art
Game covers in `/romart/` on SD card. PNG format, 160x168, 8bit.

## Changes from upstream Retro-Go
This is a modified version of [Retro-Go](https://github.com/ducalex/retro-go) by
ducalex, with the following changes:
- Added the 0v1Tech-BlockBoy-N16R8 target (ESP32-S3, 16MB flash, 8MB octal PSRAM)
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
- Game Boy Advance emulator tuned for the Pokemon titles: a Thumb/ARM JIT
  recompiler, dual-core render offload, a native MP2k audio mixer and a faster
  path for streaming the ROM off the SD card, taking Pokemon Emerald from around
  30 fps on the plain interpreter to close to a steady 60
- Removed the SNES and MSX emulators (non-commercial Snes9x and fMSX licenses)
- Removed the PC Engine and Atari Lynx emulators
- Removed all unused targets

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
