# BlockBoy Models — V1 / V2 / V3

The BlockBoy is a handheld retro game console built on the ESP32-S3, running firmware
based on [Retro-Go](https://github.com/ducalex/retro-go). Three models exist. All three
run the same software family; newer models add hardware and the features that come with it.

## At a glance

| | **V1** | **V2** | **V3** |
|---|---|---|---|
| Game systems | Game Boy, Game Boy Color, NES | 10 systems (see below) | 10 systems (see below) |
| Flash / PSRAM | 4 MB / 2 MB | 16 MB / 8 MB | 16 MB / 8 MB |
| Display | 2.8" 320×240 (ILI9341) | 2.8" 320×240 (ILI9341) | 2.8" 320×240 **IPS** (ST7789P3) on a faster 80 MHz display bus |
| Controls | D-pad, A/B, Select/Start + 4 side buttons (brightness/volume) | same as V1 | D-pad, A/B, Select/Start + **2 rotary wheels** (brightness & volume, click to press) |
| Battery-backed clock (RTC) | – | – | ✔ keeps the time while powered off |
| Status LEDs | – | – | ✔ physical LEDs, battery-level dimming |
| Magnet mode (cartridge auto-launch) | – | – | ✔ |
| Wi-Fi + web file manager | ✔ | ✔ | ✔ |
| Over-the-air firmware updates | ✔ | ✔ | ✔ |
| Bluetooth (BLE) controller support | ✔ | ✔ | ✔ |
| Game Link — wireless GB/GBC link cable | ✔ | ✔ | ✔ |
| USB-C mass storage (SD card on your PC) | ✔ | ✔ | ✔ |
| Audio over USB-C (headphone adapter) | ✔ adapter needs its own power | ✔ adapter needs its own power | ✔ |

## Game systems

| System | V1 | V2 | V3 |
|---|---|---|---|
| Nintendo Game Boy / Game Boy Color | ✔ | ✔ | ✔ |
| Nintendo Entertainment System (NES) | ✔ | ✔ | ✔ |
| Nintendo Game Boy Advance | – | ✔ | ✔ |
| Nintendo Game & Watch | – | ✔ | ✔ |
| Sega Master System / Game Gear | – | ✔ | ✔ |
| Sega Mega Drive / Genesis | – | ✔ | ✔ |
| Coleco ColecoVision | – | ✔ | ✔ |
| DOOM (including mods) | – | ✔ | ✔ |

V1 has 4 MB of flash and 2 MB of PSRAM, which is what limits it to Game Boy, Game Boy
Color and NES: there is no room for more. The Game Boy Advance emulator on V2
and V3 is identical and tuned for the Pokemon titles specifically; a custom JIT
recompiler and dual-core rendering bring those close to a steady 60 fps.

## Software features

| | V1 | V2 | V3 |
|---|---|---|---|
| **Playing** | | | |
| Save states, multiple slots per game | ✔ | ✔ | ✔ |
| Save state previews | ✔ | ✔ | ✔ |
| Turbo speed / fast forward | ✔ | ✔ | ✔ |
| Game Link — wireless GB/GBC link cable | ✔ | ✔ | ✔ |
| Magnet cartridges (auto-launch) | – | – | ✔ |
| **Launcher** | | | |
| Cover art | ✔ | ✔ | ✔ |
| Favorites and recently played | ✔ | ✔ | ✔ |
| Themes (built-in and from SD card) | ✔ | ✔ | ✔ |
| Showcase mode | ✔ | ✔ | ✔ |
| Boot animation | ✔ | ✔ | ✔ |
| **Screen** | | | |
| Scaling and filtering | ✔ | ✔ | ✔ |
| Low-battery warning | on screen | on screen | status LED |
| **Network** | | | |
| Wi-Fi with five saved networks | ✔ | ✔ | ✔ |
| Automatic clock sync, manual clock | ✔ | ✔ | ✔ |
| Web file manager | ✔ | ✔ | ✔ |
| Reachable at `blockboy.local` | ✔ | ✔ | ✔ |
| Over-the-air firmware updates | ✔ | ✔ | ✔ |
| Bluetooth controllers, remap wizard | ✔ | ✔ | ✔ |
| **USB-C** | | | |
| Mass storage (SD card on your PC) | ✔ | ✔ | ✔ |
| Audio over USB-C | ✔ adapter needs its own power | ✔ adapter needs its own power | ✔ |
| **Battery** | | | |
| Low-battery warning threshold | ✔ | ✔ | ✔ |
| Battery-backed clock (keeps time when off) | – | – | ✔ |
