# Building BlockBoy V2 (N16R8)

## Prerequisites
A working installation of [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) v5.x is required.
Builds are made and tested with **v5.5**; older 4.x releases will not compile.
Builds are made and tested with **v5.5**; older 4.x releases will not compile.

### ESP-IDF patches (optional)
Patches in `tools/patches` may improve stability:
- `sdcard-fix`: works around slow/failing SD access on some cards.
- `panic-hook`: saved crash logs to the SD card. This one only applies to
  ESP-IDF 4 — the hook it relies on no longer exists in IDF 5, so on 5.x it
  has no effect.

### exFAT SD cards (optional)
FatFs ships with exFAT disabled, so exFAT-formatted cards are not mounted by a
stock ESP-IDF. To support them, enable `FF_FS_EXFAT` (plus `FF_LBA64`) in
`components/fatfs/src/ffconf.h` of your ESP-IDF installation. Note this is a
change to the SDK, not to this project, so it has to be redone after updating
ESP-IDF. FAT32 cards work without any changes.

## Build commands

Build a flashable image:
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

Flash the generated `.img` file:
```bash
python rg_tool.py --target 0v1Tech-BlockBoy-N16R8 --port COMx install
```

Or manually with esptool:
```bash
esptool.py write_flash --flash_size detect 0x0 blockboy_*.img
```

## Development

Flash and monitor individual apps:
```bash
python rg_tool.py --target 0v1Tech-BlockBoy-N16R8 --port COMx run launcher
python rg_tool.py --target 0v1Tech-BlockBoy-N16R8 --port COMx run retro-core
python rg_tool.py --target 0v1Tech-BlockBoy-N16R8 --port COMx run gwenesis
python rg_tool.py --target 0v1Tech-BlockBoy-N16R8 --port COMx run prboom-go
python rg_tool.py --target 0v1Tech-BlockBoy-N16R8 --port COMx run gbsp
```

## Launcher images
Images used by the launcher are in `themes/default`. After editing, run `tools/gen_images.py` to regenerate `launcher/main/images.c`.

## Windows
Use `python rg_tool.py ...` instead of `./rg_tool.py ...` to ensure the correct Python interpreter is used.
