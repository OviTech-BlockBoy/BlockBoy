# Notes:
# - Stripped version for ESP32-S3 N4R2 (4MB Flash, 2MB PSRAM)
# - Only GB and GBC emulation
# - Keep at least 32KB free in a partition for future updates
# - Partitions must be 64K aligned
# - Partitions of type data are ignored when building a .fw.
# - Subtypes and offsets and size may be adjusted when building a .fw or .img

PROJECT_NAME = "BlockBoy"
PROJECT_ICON = "assets/icon.raw"
PROJECT_APPS = {
  # Project name  Type, SubType, Size
  'launcher':     [0, 0, 786432],
  'retro-core':   [0, 0, 786432],
}
