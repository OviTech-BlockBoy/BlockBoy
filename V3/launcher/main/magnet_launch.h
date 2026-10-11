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
// Matching keeps the sign, so a magnet turned over is a *different* cartridge
// and gets its own ROM. Reversing a magnet negates the whole field vector, which
// is as far from the original direction as anything can be -- the cleanest split
// the sensor can see. That doubles what each magnet position is worth and gives
// every position its mirror image back as a second, unambiguous entry.
//
// The cost is that polarity now matters at assembly: a cartridge whose magnet
// went in the other way round is simply not recognised. That is the failure to
// prefer -- it starts nothing, instead of starting somebody else's game.
//
// What separates two cartridges is therefore the magnet's position *and* which
// pole faces the sensor. Field strength is only used to decide that a cartridge
// is present at all -- it varies too much with glue and air gap to tell two
// positions apart.
//
// Every direction, stored or measured, is relative to the console's own field
// with the slot empty. That offset is not a detail: on the first V3 board it is
// 685uT against a cartridge signal of roughly 200uT, so an uncalibrated console
// reads "cartridge present" with nothing in the slot.
//
// Measuring it is "Calibrate" in the owner's own menu, not a service step, even
// though it normally happens once in production. It has to be reachable by the
// owner: the offset belongs to a particular console, so a warranty swap, a
// replaced speaker or a board that shipped before anyone thought to do it all
// leave a unit that only its owner can put right. Pairing on a console that has
// not been calibrated offers to do it there and then, because that is where the
// need shows up.

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
