# Building BlockBoy (N4R2)

## Prerequisites
A working installation of [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) v5.x is required.

### ESP-IDF Patches
Patches in `tools/patches` may improve stability:
- `panic-hook`: Saves crash logs to SD card for easier debugging.

## Build commands

Build a flashable image:
```bash
python rg_tool.py build-img all --target 0v1Tech-BlockBoy-N4R2
```

Clean build:
```bash
python rg_tool.py clean all --target 0v1Tech-BlockBoy-N4R2
python rg_tool.py build-img all --target 0v1Tech-BlockBoy-N4R2
```

## Flashing

Flash the generated `.img` file:
```bash
python rg_tool.py --target 0v1Tech-BlockBoy-N4R2 --port COM3 install
```

Or manually with esptool:
```bash
esptool.py write_flash --flash_size detect 0x0 blockboy_*.img
```

## Development

Flash and monitor individual apps:
```bash
python rg_tool.py --target 0v1Tech-BlockBoy-N4R2 --port COM3 run launcher
python rg_tool.py --target 0v1Tech-BlockBoy-N4R2 --port COM3 run retro-core
```

## Launcher images
Images used by the launcher are in `themes/default`. After editing, run `tools/gen_images.py` to regenerate `launcher/main/images.c`.

## Windows
Use `python rg_tool.py ...` instead of `./rg_tool.py ...` to ensure the correct Python interpreter is used.
