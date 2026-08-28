#pragma once

#include <rg_system.h>
#include <rg_gui.h>

// Magnet-cartridge auto-launch. Game Boy only: slots map to GB and GBC ROMs.
//
// Two board revisions, told apart at runtime by whether a TMAG5273 answers on
// the I2C bus. Both encode the magnets they see into a slot number; slot 0 is
// "nothing inserted" on either, and is reserved so the launcher stays reachable.
//
//   Old PCB: two DRV5055A1QDBZR analog Hall sensors (GPIO4 and GPIO7). Each
//   classifies into none / north / south, so slot = left * 3 + right, 0..8.
//   Each sensor keeps its own idle point and band, because the two parts do not
//   share an offset and the two ADC channels do not share a curve -- run
//   "Calibrate" with no cartridge inserted before anything else.
//
//   New PCB: one TMAG5273, three axes over I2C. GPIO7 is gone and the one
//   remaining DRV5055 is deliberately left unread -- it was fitted as a fallback
//   in case the TMAG disappointed, and it did not. Slots 9..35 are entries whose
//   meaning is learned rather than computed: pairing stores the field vector it
//   measured, and a later reading picks the entry whose direction matches.
//   The two ranges are disjoint on purpose: a device whose PCB is swapped can
//   never inherit the other revision's pairings. The TMAG needs no calibration
//   -- it reports absolute field, so a stored direction still means the same
//   thing tomorrow.
//
// Matching ignores sign, which is the point: a magnet turned over produces the
// exact opposite vector, so north-up and south-up in the same spot land on the
// same cartridge. What separates cartridges is *where* the magnet sits, since
// two positions that are not mirror images about the sensor point the field in
// measurably different directions. Field strength is only used to decide that a
// cartridge is present at all -- it varies too much with glue and air gap to
// tell two positions apart.

typedef enum {
    MAGNET_NONE  = 0,
    MAGNET_NORTH = 1,
    MAGNET_SOUTH = 2,
} magnet_state_t;

#define MAGNET_SLOT_COUNT 36

void magnet_launch_init(void);

// Reads sensors, looks up the slot's ROM path, and switches to the emulator
// app if auto-launch is enabled and the mapping resolves to an existing file.
// Does not return on successful launch. Otherwise returns and the caller
// continues into the launcher UI.
void magnet_launch_check_and_boot(void);

// Options menu entry handler. Wire this into the launcher options_handler.
// This is the owner-facing menu: switch it on, add a cartridge, see what is
// paired. Nothing here can misconfigure the sensor.
rg_gui_event_t magnet_launch_options_cb(rg_gui_option_t *option, rg_gui_event_t event);

// Diagnostics and the destructive actions. A separate entry rather than a row
// inside the menu above, because the shop menu can only show and hide entries
// that live in the launcher's tabbed list. Register it there with vis_key
// "MagnetAdv" and default_vis false.
rg_gui_event_t magnet_launch_advanced_cb(rg_gui_option_t *option, rg_gui_event_t event);
