# Notes:
# - Stripped version for ESP32-S3 N4R2 (4MB Flash, 2MB PSRAM)
# - Only GB and GBC emulation
# - Partitions must be 64K aligned
# - Partitions of type data are ignored when building a .fw.
#
# !! THE LAYOUT BELOW IS FROZEN — DO NOT CHANGE !!
#
# OTA writes app binaries to fixed offsets on the device. As soon as one
# partition grows, everything behind it shifts and an OTA update flashes to the
# wrong place. That is why the sizes here are fixed and rg_tool.py FAILS the
# build when a binary does not fit.
#
# 4MB is tight. The entire usable space is 0x3F0000 (4,129,792 bytes) from
# offset 0x10000, and two apps plus the recovery app have to fit in it:
#
#   launcher    0x1C0000  1.792K   (binary ~1.580K -> 12% marge)
#   retro-core  0x1A0000  1.664K   (binary ~1.461K -> 12% marge)
#   flasher     0x090000    576K   (binary ~476K)
#   ------------------------------
#   total       0x3F0000  4,032K   (exactly full)
#
# The flasher is deliberately sized a bit tighter than on V2/V3: that app
# rarely changes, while the two emulator partitions can use every bit of margin
# there is.
#
# So there is no room for spare partitions like on V2/V3. An extra emulator on
# V1 inevitably means a USB flash for everyone.
#

PROJECT_NAME = "BlockBoy"
PROJECT_ICON = "assets/icon.raw"
PROJECT_APPS = {
  # Project name  Type, SubType, Size
  'launcher':     [0, 0, 0x1C0000],  # 1.75M
  'retro-core':   [0, 0, 0x1A0000],  # 1.63M
}

# Recovery/flash app. Runs as 'factory', so the bootloader falls back to it
# when otadata is empty or corrupt. It is NEVER replaced via OTA.
PROJECT_FLASHER = ['flasher', 0x90000]  # 576K
