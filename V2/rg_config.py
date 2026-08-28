# Notes:
# - BlockBoy V2 (ESP32-S3 N16R8, 16MB Flash, 8MB PSRAM)
# - Partitions must be 64K aligned
# - Partitions of type data are ignored when building a .fw.
#
# !! THE LAYOUT BELOW IS FROZEN — DO NOT CHANGE !!
#
# OTA writes app binaries to fixed offsets on the device. As soon as one
# partition grows, everything behind it shifts and an OTA update flashes to the
# wrong place. That is why the sizes here are fixed and rg_tool.py FAILS the
# build when a binary does not fit, instead of silently growing the partition
# (as it used to do).
#
# App no longer fits? Then that is a breaking change: adjusting the layout is
# allowed, but devices in the field must then get a new .img over USB/web
# flasher. Also bump RG_LAYOUT_VERSION in the target config.h so old devices
# are not offered an incompatible update.
#

PROJECT_NAME = "BlockBoy"
PROJECT_ICON = "assets/icon.raw"
PROJECT_APPS = {
  # Project name  Type, SubType, Size
  'launcher':     [0, 0, 0x200000],  # 2.00M
  'retro-core':   [0, 0, 0x200000],  # 2.00M
  'prboom-go':    [0, 0, 0x1C0000],  # 1.75M
  'gwenesis':     [0, 0, 0x200000],  # 2.00M
  'gbsp':         [0, 0, 0x140000],  # 1.25M
}

# Recovery/flash app. Runs as 'factory', so the bootloader falls back to it
# when otadata is empty or corrupt. It is NEVER replaced via OTA — this is the
# safety net that straightens out a half-flashed launcher.
PROJECT_FLASHER = ['flasher', 0xA0000]  # 640K (binary ~520K)

# Reserved app partitions for future emulators.
#
# Without this reservation every new app would force a layout change, and thus
# a USB flash for everyone who already owns one. With an empty partition in the
# table a new emulator can simply ride along via OTA: the new launcher knows
# it, the manifest fills it.
#
# They sit behind the flasher and get no content in the .img (just a line in
# the partition table), so they cost nothing in download time when web
# flashing.
PROJECT_SPARE_APPS = [
  ('app5', 0x1C0000),  # 1.75M
  ('app6', 0x1C0000),  # 1.75M
]
