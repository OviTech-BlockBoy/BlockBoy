#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rg_storage.h"

// OTA updates for BlockBoy.
//
// Division of roles:
//   launcher  downloads the package to the SD card and writes a job file
//   flasher   (factory partition) reads that job and writes to flash
//
// The launcher can't overwrite itself while it is running, hence the separate
// flasher. It sits in the partition table as 'factory', so the bootloader
// falls back to it when otadata is empty or corrupt -- which doubles as the
// safety net for an interrupted update.

#define RG_OTA_PATH        RG_BASE_PATH "/update"
#define RG_OTA_JOB_FILE    RG_OTA_PATH "/update.job"

// Name of the partition holding the flasher, and that of the launcher.
#define RG_OTA_FLASHER_PARTITION "flasher"
#define RG_OTA_LAUNCHER_PARTITION "launcher"

// Namespace for OTA settings on the SD card (/BlockBoy/config/ota.json).
// Holds only the optional test override below. Deliberately no record of what
// is installed: that belongs to the device, not to the card.
#define RG_OTA_SETTINGS_NS "ota"

// Optional override of the manifest URL, from /sd/BlockBoy/config/ota.json.
// Meant for testing against a server on the local network without rebuilding
// firmware. When absent, RG_OTA_MANIFEST_URL from the target config.h applies.
#define RG_OTA_SETTING_MANIFEST_URL "ManifestURL"

// Identifies the device model in OTA packages. Deliberately NOT RG_TARGET_NAME:
// V2 and V3 share that name ("0v1Tech-BlockBoy") while being different devices
// (different display, different pinout). A package from one model flashed onto
// the other bricks the device, so this must be unique per model.
#ifndef RG_OTA_MODEL
#error "RG_OTA_MODEL must be defined in the target config.h"
#endif

// Bump whenever the partition layout changes (offsets/sizes in rg_config.py).
// A package with a different layout than the device must NEVER be flashed:
// it would write app images at the wrong offsets. The server then simply stops
// offering updates to old devices; those go through the web flasher instead.
#ifndef RG_LAYOUT_VERSION
#define RG_LAYOUT_VERSION 1
#endif

#define RG_OTA_JOB_MAGIC   "BBUPDATE"
#define RG_OTA_JOB_VERSION 1

#define RG_OTA_MAX_PARTS   8

// Deliberately NOT JSON: the flasher is the recovery path and must have as few
// dependencies as possible. Line-based text format, parseable with sscanf.
//
//   BBUPDATE 1
//   version 3.1.0
//   layout 1
//   model blockboy-v3-n16r8
//   part <partition-name> <filename> <bytes> <sha256-hex>
//   ...

typedef struct
{
    char partition[24];   // label in the partition table, e.g. "launcher"
    char filename[64];    // filename within RG_OTA_PATH
    size_t size;          // exact size in bytes
    char sha256[65];      // hex, lowercase
} rg_ota_part_t;

typedef struct
{
    char version[32];     // version of the package, e.g. "3.1.0"
    char model[48];       // must match RG_OTA_MODEL
    int layout;           // must match RG_LAYOUT_VERSION
    rg_ota_part_t parts[RG_OTA_MAX_PARTS];
    size_t parts_count;
} rg_ota_job_t;

// Reads and validates the job file. Returns false when missing, unreadable,
// wrong magic/version, or when target/layout don't belong to this device.
bool rg_ota_job_read(const char *path, rg_ota_job_t *out, char *error, size_t error_len);

// Writes a job file (used by the launcher after downloading).
bool rg_ota_job_write(const char *path, const rg_ota_job_t *job);

// Computes the sha256 of a file on the SD card. 'hex_out' must be 65 bytes.
// progress_cb may be NULL; it is called with (bytes read, total).
typedef void (*rg_ota_progress_t)(size_t done, size_t total, void *arg);
bool rg_ota_sha256_file(const char *path, char *hex_out, rg_ota_progress_t progress_cb, void *arg);
