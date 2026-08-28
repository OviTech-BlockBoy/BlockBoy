#pragma once

#include <stdbool.h>

// TMAG5273B1 3-axis Hall sensor, on the I2C bus at 0x22.
//
// Replaces the two DRV5055 analog Hall sensors. The DRV5055 is ratiometric --
// idle output is Vcc/2 and sensitivity scales with Vcc -- while the ESP32 ADC
// measures against its own internal bandgap. The two references drift apart:
// the 3.3V rail moves ~290mV over a charge cycle, which showed up as ~190 raw
// counts of offset against a magnet signal of only 30 counts. No amount of
// firmware fixes a signal six times smaller than its disturbance.
//
// The TMAG5273 carries the Hall bias, the reference and the ADC on one die, so
// what comes out over I2C is a field value that means something on its own,
// whatever the supply is doing. It also reads three axes instead of one, which
// is what lets a single part replace two.
//
// The ordering suffix encodes two different things, which is easy to get wrong
// coming from the DRV5055 (where the suffix was sensitivity):
//   letter = I2C address       B -> 0x22
//   digit  = full-scale range  1 -> +/-40mT and +/-80mT (820 / 410 LSB/mT)
//                              2 -> +/-133mT and +/-266mT (too coarse for us)

typedef struct {
    float x, y, z; // microtesla
} tmag_vec_t;

// True if a TMAG5273 answered on the bus (manufacturer ID matched). Cheap, and
// safe to call on a board where the part is not fitted -- that is how the
// caller decides between this and the legacy DRV5055 path.
bool tmag5273_probe(void);

// Probes and configures the device. Idempotent. Returns false when no sensor
// is present, in which case nothing was written to the bus.
bool tmag5273_init(void);

// One reading, in microtesla. Returns false if the device is absent or the
// transfer failed.
//
// Escalates to the +/-80mT range when +/-40mT clips, so an unexpectedly strong
// field (a stray magnet, or the speaker mounted closer than planned) degrades
// resolution instead of silently returning a clamped -- and therefore wrong --
// vector.
bool tmag5273_read(tmag_vec_t *out);

// Magnitude of a vector, in microtesla. Used for the "is a cartridge present at
// all" test, which is a plain threshold on field strength.
float tmag5273_magnitude(const tmag_vec_t *v);
