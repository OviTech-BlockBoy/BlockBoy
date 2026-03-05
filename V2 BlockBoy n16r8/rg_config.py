# Notes:
# - BlockBoy V2 (ESP32-S3 N16R8, 16MB Flash, 8MB PSRAM)
# - Keep at least 32KB free in a partition for future updates
# - Partitions must be 64K aligned
# - Partitions of type data are ignored when building a .fw.
# - Subtypes and offsets and size may be adjusted when building a .fw or .img

PROJECT_NAME = "BlockBoy"
PROJECT_ICON = "assets/icon.raw"
PROJECT_APPS = {
  # Project name  Type, SubType, Size
  'launcher':     [0, 0, 983040],
  'retro-core':   [0, 0, 983040],
  'prboom-go':    [0, 0, 786432],
  'gwenesis':     [0, 0, 983040],
}
