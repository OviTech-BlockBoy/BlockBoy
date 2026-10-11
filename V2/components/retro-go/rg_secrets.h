#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * Storage for the handful of values that have to stay secret: the Wi-Fi PSKs of
 * the customer's own network, the access point password and the file server
 * password.
 *
 * These live in NVS, in the internal flash, and deliberately NOT in the JSON
 * under /sd/BlockBoy/config. EN 18031-1 asks that a confidential stored asset
 * stays confidential, and a removable card in somebody else's laptop is the
 * easiest way to lose one. The file server used to hand the same file out over
 * the network as well.
 *
 * What this does NOT protect against is somebody with the device and a USB
 * cable: without flash encryption, flash can be read out. That is the same
 * boundary the rest of the design accepts, and encrypting the JSON instead
 * would not have changed it, because the key would have had to live in that
 * same readable flash.
 *
 * Keys are NVS keys, so at most 15 characters.
 */

// Reads a secret into `out`. Returns false when it is not there, in which case
// `out` is set to an empty string.
bool rg_secret_get(const char *key, char *out, size_t out_len);

bool rg_secret_set(const char *key, const char *value);
bool rg_secret_delete(const char *key);

// Moves a secret that an older firmware left in the settings on the SD card
// into NVS, then removes it from those settings. The card copy is removed
// whenever it exists; NVS is only written when it does not hold a value yet, so
// a value already in NVS is never overwritten by a stale card. Safe to call on
// every boot. Commits the settings itself when it removed something, so the
// plaintext is gone from the card straight away.
void rg_secret_migrate(const char *section, const char *settings_key, const char *secret_key);
