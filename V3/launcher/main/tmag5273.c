#include "tmag5273.h"

#include <rg_system.h>
#include <rg_i2c.h>

#include <math.h>
#include <stdint.h>

// 7-bit address. The datasheet's I2C address table also lists 44h/45h as the
// 8-bit write/read forms of the same device; rg_i2c wants the 7-bit value, and
// mixing those up is the classic reason a part "does not answer".
#define TMAG_ADDR              0x22

// Register map, datasheet section 8.1.
#define REG_DEVICE_CONFIG_1    0x00
#define REG_DEVICE_CONFIG_2    0x01
#define REG_SENSOR_CONFIG_1    0x02
#define REG_SENSOR_CONFIG_2    0x03
#define REG_INT_CONFIG_1       0x08
#define REG_MANUFACTURER_ID    0x0E // LSB at 0x0E, MSB at 0x0F
#define REG_X_MSB_RESULT       0x12 // X, Y, Z as MSB+LSB pairs -- 6 bytes

#define MANUFACTURER_ID        0x5449 // reset values: MSB 54h, LSB 49h

// MAG_TEMPCO = 1h: compensate for the 0.12%/degC coefficient of NdFeB. Our
// cartridge magnets are neodymium, and so is the speaker's, so this correction
// happens to fit both the signal and the largest part of the ambient offset.
// CONV_AVG = 5h: 32x averaging. Not optional -- at 1x the X/Y noise is 125uT,
// at 32x it is 22uT. Costs ~825us per conversion, which is free for a reading
// we take once per cold boot.
#define DEVICE_CONFIG_1_VALUE  ((1 << 5) | (5 << 2))

// LP_LN = 1 (low-noise mode) | OPERATING_MODE = 2h (continuous).
// Continuous also sidesteps the slow-VCC-ramp erratum in the datasheet: the
// wake-and-sleep cycle it prescribes after power-on is only needed for sleep
// mode, explicitly not for standby or continuous.
#define DEVICE_CONFIG_2_VALUE  ((1 << 4) | 2)

// MAG_CH_EN = 7h: X, Y and Z. We need the whole vector, not one axis.
#define SENSOR_CONFIG_1_VALUE  (7 << 4)

// MASK_INTB = 1. Required rather than optional on this board: INT is tied to
// GND, and the datasheet says to mask the pin when it is grounded so the device
// never tries to drive it low against the short.
#define INT_CONFIG_1_VALUE     0x01

// SENSOR_CONFIG_2: bit1 = X_Y_RANGE, bit0 = Z_RANGE. Both moved together --
// comparing axes only makes sense when they share a scale.
#define RANGE_40MT             0x00
#define RANGE_80MT             0x03

// Sensitivity in LSB/mT, datasheet 5.7 (shared by A1/B1/C1/D1).
#define SENS_40MT              820.0f
#define SENS_80MT              410.0f

// Results are 16-bit signed, so the rail is +/-32768. Escalate before reaching
// it: a value sitting exactly at the rail is indistinguishable from a much
// larger field that got clamped.
#define CLIP_THRESHOLD         31000

static bool initialized = false;
static uint8_t range = RANGE_40MT;

static bool write_reg(uint8_t reg, uint8_t value)
{
    return rg_i2c_write_byte(TMAG_ADDR, reg, value);
}

bool tmag5273_probe(void)
{
    rg_i2c_init();

    uint8_t id[2];
    if (!rg_i2c_read(TMAG_ADDR, REG_MANUFACTURER_ID, id, sizeof(id)))
        return false;

    uint16_t value = (uint16_t)(id[1] << 8) | id[0];
    if (value != MANUFACTURER_ID)
    {
        RG_LOGW("TMAG5273: something answered at 0x%02X but ID was 0x%04X, expected 0x%04X",
                TMAG_ADDR, value, MANUFACTURER_ID);
        return false;
    }
    return true;
}

bool tmag5273_init(void)
{
    if (initialized)
        return true;
    if (!tmag5273_probe())
        return false;

    // Order matters for the first write only: mask INT before anything can
    // start a conversion, so the device never asserts a pin that is grounded.
    bool ok = write_reg(REG_INT_CONFIG_1, INT_CONFIG_1_VALUE);
    ok = write_reg(REG_SENSOR_CONFIG_2, range) && ok;
    ok = write_reg(REG_SENSOR_CONFIG_1, SENSOR_CONFIG_1_VALUE) && ok;
    ok = write_reg(REG_DEVICE_CONFIG_1, DEVICE_CONFIG_1_VALUE) && ok;
    ok = write_reg(REG_DEVICE_CONFIG_2, DEVICE_CONFIG_2_VALUE) && ok;

    if (!ok)
    {
        RG_LOGE("TMAG5273: found the device but configuration failed");
        return false;
    }

    // First continuous conversion at 32x averaging takes ~825us. Wait for it,
    // otherwise the first read returns the power-on zeros and looks like "no
    // cartridge" on exactly the boot where it matters.
    rg_task_delay(5);

    initialized = true;
    RG_LOGI("TMAG5273 ready at 0x%02X, range %s", TMAG_ADDR,
            range == RANGE_40MT ? "+/-40mT" : "+/-80mT");
    return true;
}

bool tmag5273_read(tmag_vec_t *out)
{
    if (!initialized && !tmag5273_init())
        return false;

    uint8_t buf[6];
    if (!rg_i2c_read(TMAG_ADDR, REG_X_MSB_RESULT, buf, sizeof(buf)))
        return false;

    int16_t raw[3];
    for (int i = 0; i < 3; ++i)
        raw[i] = (int16_t)(((uint16_t)buf[i * 2] << 8) | buf[i * 2 + 1]);

    // One escalation only: the guard means a re-read on the wide range returns
    // whatever it returns rather than recursing again.
    if (range == RANGE_40MT)
    {
        for (int i = 0; i < 3; ++i)
        {
            if (raw[i] > CLIP_THRESHOLD || raw[i] < -CLIP_THRESHOLD)
            {
                RG_LOGW("TMAG5273: axis %d clipped at %d, widening to +/-80mT", i, raw[i]);
                range = RANGE_80MT;
                write_reg(REG_SENSOR_CONFIG_2, range);
                rg_task_delay(5);
                return tmag5273_read(out);
            }
        }
    }

    const float scale = 1000.0f / ((range == RANGE_40MT) ? SENS_40MT : SENS_80MT);
    out->x = raw[0] * scale;
    out->y = raw[1] * scale;
    out->z = raw[2] * scale;
    return true;
}

float tmag5273_magnitude(const tmag_vec_t *v)
{
    return sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
}
