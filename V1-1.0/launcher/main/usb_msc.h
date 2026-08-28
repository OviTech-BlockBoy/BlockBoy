#pragma once

#include <stdbool.h>

/**
 * USB Mass Storage Class (MSC) module for SD card access via USB.
 *
 * This allows the device to appear as a USB flash drive when connected
 * to a computer, providing direct access to the SD card contents.
 *
 * IMPORTANT: The SD card cannot be used by the device while USB MSC
 * is active. The emulator and file browser will not work during this time.
 */

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
// USB MSC requires ESP32-S2, S3, or later with USB OTG support
#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3)
#define RG_USB_MSC_SUPPORTED 1
#endif
#endif

#ifndef RG_USB_MSC_SUPPORTED
#define RG_USB_MSC_SUPPORTED 0
#endif

/**
 * Initialize the USB MSC subsystem.
 * This should be called once at startup.
 */
void usb_msc_init(void);

/**
 * Start USB MSC mode.
 *
 * This will:
 * 1. Unmount the SD card from the filesystem
 * 2. Initialize USB as a Mass Storage Device
 * 3. Expose the SD card to the connected computer
 *
 * The function blocks until USB is disconnected or usb_msc_stop() is called.
 *
 * @return true if USB MSC was started successfully, false on error
 */
bool usb_msc_start(void);

/**
 * Stop USB MSC mode and return to normal operation.
 *
 * This will:
 * 1. Disconnect from USB host
 * 2. Deinitialize USB MSC
 * 3. Remount the SD card for normal use
 *
 * @return true if stopped successfully, false on error
 */
bool usb_msc_stop(void);

/**
 * Check if USB MSC mode is currently active.
 *
 * @return true if USB MSC is active, false otherwise
 */
bool usb_msc_is_active(void);

/**
 * Run USB MSC mode with UI feedback.
 *
 * This is the main entry point for the menu option.
 * Shows a dialog, handles the USB connection, and returns when done.
 */
void usb_msc_run_with_ui(void);
