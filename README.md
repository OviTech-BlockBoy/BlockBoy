# BlockBoy by OviTech

Firmware for the BlockBoy, a handheld retro game console built on the ESP32-S3.
Based on [Retro-Go](https://github.com/ducalex/retro-go) by ducalex.

The BlockBoy turns the LEGO® Game Boy™ set (#72046) into a working handheld. This
repository holds the firmware, the web flasher and the update manifests.

## What is in here

| | |
|---|---|
| `V1` `V2` `V3` | Current firmware source, one folder per model |
| `V1-1.0` `V2-1.0` | Source of the 1.0 release, kept because that firmware is still offered |
| `Flash/` | The web flasher, served at [blockboy.nl/pages/firmware](https://blockboy.nl/pages/firmware) |
| `ota/` | Update manifests the devices themselves fetch |

## Models

|  | V1 | V2 | V3 |
|---|---|---|---|
| Chip | ESP32-S3 N4R2 | ESP32-S3 N16R8 | ESP32-S3 N16R8 |
| Flash / PSRAM | 4 MB / 2 MB | 16 MB / 8 MB | 16 MB / 8 MB |
| Display | 2.8" ILI9341 | 2.8" ILI9341 | 2.8" IPS ST7789 |
| Systems | Game Boy, Game Boy Color, NES | 10 systems | 10 systems |
| Build target | `0v1Tech-BlockBoy-N4R2` | `0v1Tech-BlockBoy-N16R8` | `0v1Tech-BlockBoy-N16R8` |

See [MODELS.md](V3/MODELS.md) for the full comparison.

V2 and V3 use the same chip and the same amount of flash, so they cannot be told
apart that way. Every V3 is given a marker in eFuse `BLOCK_KEY0` during
production; an unburned block reads zero, which identifies a V2. Both the
firmware and the web flasher read that marker, so a device can never be given
firmware meant for the other model.

## Updating a device

**V1 and V2 owners** run the [web flasher](https://blockboy.nl/pages/firmware)
once over USB. Every version after that arrives over Wi-Fi from the menu, under
Settings → Wi-Fi → Firmware update.

**V3 owners** already have the latest firmware and update from the menu. The web
flasher is only needed if something goes wrong.

Updates are checked before anything is written: a package for a different model
or a different partition layout is refused, and every file is verified against a
SHA-256 from the manifest.

## Building

Requires ESP-IDF v5.5.

```bash
cd V3
python rg_tool.py build-img all --target 0v1Tech-BlockBoy-N16R8
```

Use `--target 0v1Tech-BlockBoy-N4R2` for V1. The result is a single `.img` that
can be written to address 0 with esptool or through the web flasher.

See [BUILDING.md](V3/BUILDING.md) for the details.

## Credits

- [Retro-Go](https://github.com/ducalex/retro-go) by ducalex
- BlockBoy hardware and firmware by [OviTech](https://blockboy.nl)

BlockBoy is an independent kit and is not affiliated with the LEGO Group or
Nintendo.

## License

GNU General Public License v2.0 — see [COPYING](V3/COPYING).
