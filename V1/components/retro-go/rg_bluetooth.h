#pragma once

#include <stdbool.h>
#include <stdint.h>

// BLE gamepad support (HID over GATT, aka HOGP). The ESP32-S3 has no Bluetooth
// Classic, so only controllers with a BLE mode work (e.g. ShanWan Q36 with the
// mode switch on "D" / "Q36 for Android"). Compiled in when CONFIG_BT_NIMBLE_ENABLED
// is set in sdkconfig; stubbed out otherwise so callers don't need #ifdefs.

#define RG_SETTING_BT_CONTROLLER "BTController"

typedef enum
{
    RG_BT_STATE_OFF = 0,
    RG_BT_STATE_SCANNING,   // Stack running, looking for a controller
    RG_BT_STATE_CONNECTING, // Found a controller, connecting/discovering
    RG_BT_STATE_CONNECTED,  // Input reports flowing
} rg_bt_state_t;

// True if BLE support is compiled into this build
bool rg_bluetooth_supported(void);
// Start the BLE stack and background scan/connect task. Safe to call twice.
bool rg_bluetooth_init(void);
// Stop everything and release the radio. Safe to call when not initialized.
void rg_bluetooth_deinit(void);
rg_bt_state_t rg_bluetooth_get_state(void);
// Name of the connected controller ("" when not connected)
const char *rg_bluetooth_get_device_name(void);
// Current key state of the BLE gamepad as a mask of rg_key_t
uint32_t rg_bluetooth_read_gamepad(void);
// Remove all stored pairings (the controller will need to re-pair)
bool rg_bluetooth_forget_bonds(void);

// Button remapping. "Usage" is the controller's physical button number
// (1-16, HID button usage). The map assigns an rg_key_t to each usage.
#define RG_BT_MAX_BUTTONS 16
// Raw pressed buttons: bit N = usage N+1 currently held
uint32_t rg_bluetooth_read_raw_buttons(void);
// Same as read_gamepad but ignores capture mode (for the test screen)
uint32_t rg_bluetooth_peek_gamepad(void);
// While capturing, rg_bluetooth_read_gamepad() returns 0 so button presses
// don't leak into the UI during the remap dialog
void rg_bluetooth_set_capture(bool on);
// Apply + persist a new usage->key map (entries with 0 are unassigned)
void rg_bluetooth_set_button_map(const uint32_t map[RG_BT_MAX_BUTTONS]);
// Current usage->key map (array of RG_BT_MAX_BUTTONS entries)
const uint32_t *rg_bluetooth_get_button_map(void);
// Restore + persist the default map
void rg_bluetooth_reset_button_map(void);
