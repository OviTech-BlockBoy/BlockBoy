# BlockBoy by OviTech

A custom retro gaming handheld based on [Retro-Go](https://github.com/ducalex/retro-go) by ducalex, running on the ESP32-S3.

## Versions

### V1 - BlockBoy N4R2
- **Hardware:** ESP32-S3 with 4MB Flash / 2MB PSRAM
- **Display:** ILI9341 320x240
- **Emulators:** Game Boy, Game Boy Color
- **Build target:** `0v1Tech-BlockBoy-N4R2`

### V2 - BlockBoy N16R8
- **Hardware:** ESP32-S3 with 16MB Flash / 8MB PSRAM
- **Display:** ILI9341 320x240
- **Emulators:** NES, Game Boy, Game Boy Color, Game & Watch, Sega Master System, Game Gear, Mega Drive/Genesis, ColecoVision, DOOM
- **Build target:** `0v1Tech-BlockBoy-N16R8`

## BlockBoy Features
Custom additions on top of [Retro-Go](https://github.com/ducalex/retro-go):
- Custom ESP32-S3 hardware target with ILI9341 display
- USB Mass Storage mode (access SD card via USB-C)
- Boot animation options (blocks, scroll, off)
- Showcase / demo mode
- Battery monitoring

## Building

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) v4.4 - v5.x.

```bash
# V1 (N4R2)
python rg_tool.py build-img all --target 0v1Tech-BlockBoy-N4R2

# V2 (N16R8)
python rg_tool.py build-img all --target 0v1Tech-BlockBoy-N16R8
```

See [V1 Build Guide](V1-BlockBoy-N4R2/BUILDING.md) and [V2 Build Guide](V2-BlockBoy-N16R8/BUILDING.md) for details.

## Flashing

Replace `COMx` with your actual serial port (check Device Manager → Ports).

```bash
# V1
python rg_tool.py --target 0v1Tech-BlockBoy-N4R2 --port COMx install

# V2
python rg_tool.py --target 0v1Tech-BlockBoy-N16R8 --port COMx install
```

## Credits
- [Retro-Go](https://github.com/ducalex/retro-go) by ducalex
- BlockBoy hardware and firmware by [OviTech](https://0v1tech.com)

## License
This project is licensed under the GNU General Public License v2.0 - see the [COPYING](COPYING) file in each version folder for details.
