#include "magnet_launch.h"
#include "applications.h"
#include "gui.h"
#include "tmag5273.h"

#include <rg_system.h>
#include <rg_gui.h>

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <strings.h>

#ifdef ESP_PLATFORM
#include <driver/adc.h>
#endif

// DRV5055A1QDBZR linear Hall sensor, 3.3V supply.
//   Quiescent output is Vcc/2 = 1.65V, which lands *somewhere near* raw 2050 at
//   12-bit / ATTEN_DB_12 -- "near" because the exact value depends on the part's
//   own offset and on that ADC channel's non-linearity, and the two sensors do
//   not agree. So the idle point is not hardcoded: each sensor stores its own
//   measured centre ("Calibrate" with no cartridge inserted) plus its own band,
//   and a reading only counts as N or S once it moves more than <band> counts
//   away from that centre.
//
//   Sensitivity at 3.3V is ~66 mV/mT (ratiometric down from 100 mV/mT @ 5V), so
//   a 5 mT magnet swings the output ~330 mV = ~410 raw counts. The default band
//   of 40 counts (~32 mV, ~0.5 mT) is deliberately well below that: it trips on
//   weak/distant magnets, at the cost of sitting closer to the noise floor.
//
//   Pin map: left sensor = GPIO4 = ADC1_CH3, right sensor = GPIO7 = ADC1_CH6.
//   config.h deliberately leaves both pins unassigned for this.
#define MAGNET_ADC_CH_L    ADC_CHANNEL_3
#define MAGNET_ADC_CH_R    ADC_CHANNEL_6

#define MAGNET_SENSORS         2
#define MAGNET_CENTER_DEFAULT  2048
#define MAGNET_BAND_DEFAULT    40
#define MAGNET_BAND_STEP       10
#define MAGNET_BAND_MIN        10
#define MAGNET_BAND_MAX        2047
#define MAGNET_SAMPLES         16
#define MAGNET_CAL_ROUNDS      8

// An idle reading outside this window is not Vcc/2: the sensor is unpowered or
// unconnected, or a magnet was sitting on it during calibration. Worth warning
// about, because everything downstream silently misbehaves if the centre is wrong.
#define MAGNET_CENTER_MIN      1400
#define MAGNET_CENTER_MAX      2700

// Field strength at which one TMAG5273 axis counts as a magnet rather than as
// ambient. The noise floor is ~8uT after averaging and the earth field is ~50uT,
// while a cartridge magnet measures several hundred -- so this sits clear of
// both while still catching a magnet that sits a little further off than
// intended. Found empirically on the first TMAG board. Adjustable from "Show
// readings" because magnet size and air gap are a mechanical choice.
#define MAGNET_FIELD_DEFAULT   150 // microtesla
#define MAGNET_FIELD_STEP      50
// The DRV5055 path averages MAGNET_SAMPLES ADC reads; do the same for the TMAG
// so the two are comparably steady. The part already averages 32x internally --
// this is on top of that, and mainly stops the on-screen numbers from dancing.
// Averaging N reads divides the remaining noise by sqrt(N): ~22uT becomes ~8uT.
#define MAGNET_FIELD_SAMPLES   8

// Learned entries live at slot 9 and up; 0..8 stay reserved for the computed
// slots of the two-DRV5055 board.
#define MAGNET_FP_FIRST        9

// How closely a reading must point along a stored direction to count as that
// cartridge, as |cos| between the two vectors. Measured on the first TMAG board:
// the same magnet turned over scores 0.997-0.999, while two different magnet
// positions score 0.80-0.86. 0.95 (18 degrees) sits between those with room on
// both sides, and leaves far more than the ~4 degrees of angular noise a 50uT
// wobble puts on an 800uT vector.
#define MAGNET_FP_MATCH        0.95f
// A best score means nothing on its own: 0.970 beating 0.940 is a six-degree
// decision, and the wrong side of it starts somebody else's game. So the winner
// also has to beat the runner-up by this much, or nothing launches.
#define MAGNET_FP_MARGIN       0.02f
#define MAGNET_FIELD_MIN       50
#define MAGNET_FIELD_MAX       5000

// Samples averaged for the zero measurement. Eight times what a normal reading
// takes, because every later reading is expressed relative to this one -- an
// error here is an error in all of them, for as long as it stands.
#define MAGNET_ZERO_SAMPLES    64

#define MAGNET_NS              "magnet"
#define MAGNET_KEY_ENABLE      "Enable"
#define MAGNET_KEY_FIELD       "FieldUT"
// Ambient field with the slot empty, as "x,y,z" in microtesla. This is not a
// small correction. Measured on the first V3 board it is 685uT -- fourteen times
// the earth field -- while the cartridge magnet only tilts that vector by 14 to
// 20 degrees, which works out at 170-240uT of actual signal. Left unsubtracted,
// an empty console reads far above any sane presence threshold and matches
// whichever pairing happens to lie closest to the board's own field.
#define MAGNET_KEY_ZERO        "Zero"
// Bumped to 2 when stored directions became zero-relative. Entries written by an
// older build still carry the offset; subtracting today's zero from them, once,
// is exactly the correction they never got.
#define MAGNET_KEY_VER         "Ver"
#define MAGNET_VER_ZEROED      2

// "Slot" + an int + "App" + NUL. Sized for any int so the compiler can see that
// the slot keys never truncate.
#define MAGNET_KEY_LEN         24

// Sentinel stored in a slot mapping to mean "start showcase mode" instead of
// launching one ROM. Not a valid file path, so it never collides with a ROM.
#define MAGNET_SHOWCASE_TOKEN  "@showcase"

// Index 0 is the left sensor throughout -- labels, settings keys, and the slot
// number, which is (left * 3 + right). The pin numbers stay in the pin-map
// comment above and are never shown in the UI.
// The settings keys are named L/R rather than 1/2 so that an install carrying
// the old Center1/Band1 values can't silently apply them to the wrong sensor:
// those keys are simply not read any more, and both sensors start from defaults.
static const int         magnet_adc_ch[MAGNET_SENSORS]      = {MAGNET_ADC_CH_L, MAGNET_ADC_CH_R};
static const char *const magnet_key_center[MAGNET_SENSORS]  = {"CenterL", "CenterR"};
static const char *const magnet_key_band[MAGNET_SENSORS]    = {"BandL", "BandR"};
static const char *const magnet_sensor_name[MAGNET_SENSORS] = {"Sensor L", "Sensor R"};

static bool initialized = false;

// True when a TMAG5273 answered on the I2C bus. That is the whole board-revision
// test: the part exists only on the newer V3 PCB, and an I2C ACK is something a
// board without it cannot fake -- unlike guessing from a floating ADC pin. On
// that board GPIO7 (the right-hand DRV5055) is not routed either, so the left
// DRV5055 on GPIO4 is all that remains of the analog pair.
static bool tmag_present = false;

// Number of DRV5055 channels this board reads. Zero on the TMAG board: one part
// is still fitted there as a fallback for a TMAG that disappointed, but the TMAG
// did not, and a ratiometric sensor read against the ESP's own reference only
// adds a supply-dependent offset to an otherwise absolute measurement.
static int magnet_adc_count(void)
{
    if (tmag_present)
        return 0;
#if defined(RG_GPIO_LCD_PWR) && RG_GPIO_LCD_PWR == GPIO_NUM_7
    // GPIO7 drives the panel supply switch on this board revision. We only get here
    // when the TMAG failed to answer, but claiming GPIO7 as an ADC input would fight
    // that output and black the screen out -- a far worse failure than losing one
    // Hall channel. The left DRV5055 on GPIO4 keeps magnet launch working.
    return 1;
#else
    return MAGNET_SENSORS;
#endif
}

// Defined further down with the rest of the zero handling, but needed here:
// init is where the stored offset is loaded, and nothing may read a field value
// before that has happened.
static void zero_refresh(void);

void magnet_launch_init(void)
{
#ifdef ESP_PLATFORM
    if (initialized)
        return;

    tmag_present = tmag5273_init();
    zero_refresh();

    adc1_config_width(ADC_WIDTH_MAX - 1);
    // Only touch channels the board has. Claiming GPIO7 as an ADC input on the
    // new PCB would tie up a pin that is free for something else there.
    for (int s = 0; s < magnet_adc_count(); ++s)
    {
        adc1_config_channel_atten(magnet_adc_ch[s], ADC_ATTEN_DB_12);
        (void)adc1_get_raw((adc1_channel_t)magnet_adc_ch[s]);
    }
    initialized = true;

    RG_LOGI("Magnet sensors: %s", tmag_present ? "TMAG5273 (I2C)"
                                               : "2x DRV5055 (ADC)");
#endif
}

// Returns the averaged raw value, or -1 if every sample failed.
static int magnet_read_avg(int sensor)
{
#ifdef ESP_PLATFORM
    // Not fitted on this board. -1 is the existing "no usable reading" value and
    // classifies as MAGNET_NONE, so the slot arithmetic stays valid.
    if (sensor >= magnet_adc_count())
        return -1;

    int sum = 0, count = 0;
    for (int i = 0; i < MAGNET_SAMPLES; ++i)
    {
        // adc1_get_raw returns -1 on failure; averaging that in would drag the
        // result towards NORTH and look like a magnet.
        int value = adc1_get_raw((adc1_channel_t)magnet_adc_ch[sensor]);
        if (value >= 0)
        {
            sum += value;
            count++;
        }
    }
    return count ? sum / count : -1;
#else
    (void)sensor;
    return MAGNET_CENTER_DEFAULT;
#endif
}

static int magnet_get_center(int sensor)
{
    return (int)rg_settings_get_number(MAGNET_NS, magnet_key_center[sensor], MAGNET_CENTER_DEFAULT);
}

static int magnet_get_band(int sensor)
{
    return (int)rg_settings_get_number(MAGNET_NS, magnet_key_band[sensor], MAGNET_BAND_DEFAULT);
}

static magnet_state_t magnet_classify(int raw, int sensor)
{
    if (raw < 0)
        return MAGNET_NONE;
    int center = magnet_get_center(sensor);
    int band = magnet_get_band(sensor);
    if (raw <= center - band) return MAGNET_NORTH;
    if (raw >= center + band) return MAGNET_SOUTH;
    return MAGNET_NONE;
}

static int magnet_field_threshold(void)
{
    return (int)rg_settings_get_number(MAGNET_NS, MAGNET_KEY_FIELD, MAGNET_FIELD_DEFAULT);
}

// Everything one sample sees, so the menus can show the numbers behind a slot
// without reading the sensors a second time and getting a different answer.
typedef struct {
    int            raw[MAGNET_SENSORS];  // DRV5055 ADC counts, -1 when not fitted
    magnet_state_t adc[MAGNET_SENSORS];
    tmag_vec_t     field;                // microtesla, raw, only when field_valid
    tmag_vec_t     net;                  // field minus the stored zero
    bool           field_valid;
    int            nearest;              // closest entry, threshold or not
    float          match;                // its signed cos to this reading
    float          runner_up;            // the next closest entry's signed cos
    int            flipped;              // entry this reading best matches reversed
    float          flip_match;           // that entry's score, as a positive number
} magnet_reading_t;

// Stored direction of one learned cartridge, as the microtesla vector that was
// measured when it was paired. Kept unnormalised so the saved value is readable
// and can be compared against a fresh reading by eye.
static void fp_key(int slot, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "Fp%d", slot);
}

// One "x,y,z" setting, parsed. Shared by the per-slot directions and by the zero
// measurement, which is stored in exactly the same shape.
static bool vec_load(const char *key, tmag_vec_t *out)
{
    char *text = rg_settings_get_string(MAGNET_NS, key, NULL);
    if (!text)
        return false;

    // strtof, not sscanf("%f"): this build uses newlib's nano formatting
    // (CONFIG_NEWLIB_NANO_FORMAT), and its scanf leaves floating point out. A
    // "%f" here parses nothing at all, silently, and every cartridge then looks
    // unknown. printf's float support is a separate switch and does work.
    char *p = text;
    float v[3];
    bool ok = true;

    for (int i = 0; i < 3 && ok; ++i)
    {
        char *end = NULL;
        v[i] = strtof(p, &end);
        if (end == p)
        {
            ok = false;
            break;
        }
        p = end;
        if (i < 2)
        {
            while (*p == ' ')
                p++;
            if (*p == ',')
                p++;
            else
                ok = false;
        }
    }

    if (!ok)
        RG_LOGW("Magnet: cannot parse %s = '%s'", key, text);
    free(text);
    if (!ok)
        return false;

    out->x = v[0];
    out->y = v[1];
    out->z = v[2];
    return true;
}

static void vec_save(const char *key, const tmag_vec_t *v)
{
    char text[48];
    snprintf(text, sizeof(text), "%.0f,%.0f,%.0f", v->x, v->y, v->z);
    rg_settings_set_string(MAGNET_NS, key, text);
}

static bool fp_load(int slot, tmag_vec_t *out)
{
    char key[MAGNET_KEY_LEN];
    fp_key(slot, key, sizeof(key));
    if (!vec_load(key, out))
        return false;
    // An all-zero direction carries no information and would score 0 against
    // everything, so it reads as "this slot is free" rather than as a match.
    return tmag5273_magnitude(out) > 0.0f;
}

static void fp_store(int slot, const tmag_vec_t *v)
{
    char key[MAGNET_KEY_LEN];
    fp_key(slot, key, sizeof(key));
    vec_save(key, v);
}

// ----- Zero (ambient) measurement -----

// Cached: the live diagnostics ask for it several times per frame, and it is
// needed on the boot path before anything else happens. magnet_take_zero() is
// the only writer, and it refreshes this.
static tmag_vec_t zero_vec = {0.0f, 0.0f, 0.0f};
static bool       zero_known = false;

static void zero_refresh(void)
{
    zero_known = vec_load(MAGNET_KEY_ZERO, &zero_vec);
    if (!zero_known)
        zero_vec = (tmag_vec_t){0.0f, 0.0f, 0.0f};
}

// The cartridge's own contribution: what the sensor sees now, minus what it sees
// with the slot empty. On a console that has never been zeroed this is the raw
// vector, so such a unit behaves exactly as it did before rather than becoming
// differently wrong -- but it is wrong, and the boot path says so in the log.
static tmag_vec_t zero_subtract(const tmag_vec_t *v)
{
    return (tmag_vec_t){v->x - zero_vec.x, v->y - zero_vec.y, v->z - zero_vec.z};
}

// Defined with the rest of the menu code, but pairing needs it: an uncalibrated
// console discovers the fact exactly when somebody tries to pair, and sending
// them off to find a menu item is a worse answer than doing it there.
static bool magnet_take_zero(void);

// Signed cos between two field vectors: +1 the same direction, -1 exactly
// opposite, 0 at right angles.
//
// The sign is kept deliberately. Turning a magnet over negates the whole vector,
// so the reversed cartridge scores about -1 against the entry it was paired
// from: as far away as the metric goes, and therefore free to be paired to a
// different ROM. Taking fabsf() here -- which is what this used to do -- folds
// those two states onto each other and throws away half the sphere.
static float fp_similarity(const tmag_vec_t *a, const tmag_vec_t *b)
{
    float na = tmag5273_magnitude(a);
    float nb = tmag5273_magnitude(b);
    if (na <= 0.0f || nb <= 0.0f)
        return 0.0f;
    return (a->x * b->x + a->y * b->y + a->z * b->z) / (na * nb);
}

// Closest stored entry, whether or not it clears MAGNET_FP_MATCH -- the caller
// applies that. `score` gets its similarity and `second` the runner-up's, and
// the gap between those two is the real measure of how safe a match is: a 0.99
// that beats a 0.98 is a coin flip, a 0.99 that beats a 0.86 is not.
//
// `flip_slot` and `flip_score` report the same search run against the reversed
// reading. Now that polarity distinguishes cartridges, "no match, but a strong
// match if you turn it over" is the single most likely thing to go wrong at
// assembly, and it is worth being able to say so instead of shrugging.
static int fp_nearest(const tmag_vec_t *v, float *score, float *second,
                      int *flip_slot, float *flip_score)
{
    int best_slot = 0, worst_slot = 0;
    // Below any real cos, so the first entry examined always wins -- with signed
    // scores, initialising to 0 would silently ignore every entry the reading
    // points away from.
    float best = -2.0f, next = -2.0f, worst = 2.0f;

    for (int slot = MAGNET_FP_FIRST; slot < MAGNET_SLOT_COUNT; ++slot)
    {
        tmag_vec_t ref;
        if (!fp_load(slot, &ref))
            continue;
        float s = fp_similarity(v, &ref);
        if (s > best)
        {
            next = best;
            best = s;
            best_slot = slot;
        }
        else if (s > next)
        {
            next = s;
        }
        if (s < worst)
        {
            worst = s;
            worst_slot = slot;
        }
    }

    if (!best_slot)
        best = next = 0.0f;    // nothing stored at all
    else if (next < -1.0f)
        next = -1.0f;          // one entry only: nothing to be confused with

    if (score)
        *score = best;
    if (second)
        *second = next;
    if (flip_slot)
        *flip_slot = worst_slot;
    if (flip_score)
        *flip_score = worst_slot ? -worst : 0.0f;
    return best_slot;
}

// First entry with no direction stored, or 0 when they are all taken.
static int fp_first_free(void)
{
    for (int slot = MAGNET_FP_FIRST; slot < MAGNET_SLOT_COUNT; ++slot)
    {
        tmag_vec_t ref;
        if (!fp_load(slot, &ref))
            return slot;
    }
    return 0;
}

static int magnet_read(magnet_reading_t *out)
{
    memset(out, 0, sizeof(*out));

    for (int s = 0; s < MAGNET_SENSORS; ++s)
    {
        out->raw[s] = magnet_read_avg(s);
        out->adc[s] = magnet_classify(out->raw[s], s);
    }

    if (!tmag_present)
        return (int)out->adc[0] * 3 + (int)out->adc[1];

    tmag_vec_t sum = {0.0f, 0.0f, 0.0f};
    int got = 0;
    for (int i = 0; i < MAGNET_FIELD_SAMPLES; ++i)
    {
        // Paced, not back-to-back. One conversion at 32x averaging takes ~825us
        // while a six-byte I2C read takes ~250us, so an unpaced burst re-reads
        // the same conversion three times over and averages nothing at all --
        // the sqrt(N) above only holds for samples that are actually distinct.
        // FreeRTOS runs at 1kHz on this build, so 2ms really is 2ms.
        if (i)
            rg_task_delay(2);
        tmag_vec_t v;
        if (!tmag5273_read(&v))
            continue;
        sum.x += v.x;
        sum.y += v.y;
        sum.z += v.z;
        got++;
    }

    out->field_valid = (got > 0);
    if (out->field_valid)
    {
        out->field.x = sum.x / got;
        out->field.y = sum.y / got;
        out->field.z = sum.z / got;
        out->net = zero_subtract(&out->field);
    }

    if (!out->field_valid)
        return 0;

    // Strength decides whether a cartridge is there at all; direction decides
    // which one. Below the threshold the direction is just noise pointing
    // somewhere, so there is nothing to match against.
    //
    // Both tests run on the zero-corrected vector. The raw one is dominated by
    // the console's own magnets: it clears any presence threshold with the slot
    // empty, and it points in very nearly the same direction whether a cartridge
    // is in there or not.
    if (tmag5273_magnitude(&out->net) < (float)magnet_field_threshold())
        return 0;

    out->nearest = fp_nearest(&out->net, &out->match, &out->runner_up,
                              &out->flipped, &out->flip_match);

    // Two tests, not one. The score says the reading looks like this entry; the
    // margin says it does not look equally like a different one. Without the
    // second, a console starts the wrong game and nothing in the log explains it.
    if (out->match < MAGNET_FP_MATCH)
        return 0;
    if (out->match - out->runner_up < MAGNET_FP_MARGIN)
        return 0;
    return out->nearest;
}

// The 16 samples of one average are taken back-to-back, so they average
// correlated noise rather than cancelling it. The boot decision launches a game
// and can't be undone, so require two passes ~25ms apart to agree. Returns -1
// when they disagree (reading is drifting, or a cartridge is mid-insertion).
static int magnet_read_stable(magnet_reading_t *out)
{
    int slot = magnet_read(out);
    rg_task_delay(25);

    magnet_reading_t again;
    if (magnet_read(&again) != slot)
        return -1;

    return slot;
}

static const char *state_name(magnet_state_t s)
{
    switch (s) {
        case MAGNET_NORTH: return "N";
        case MAGNET_SOUTH: return "S";
        default:           return "-";
    }
}

// Human-readable slot. Old-PCB slots show the two DRV5055 sensors as (L,R);
// TMAG slots show the three field axes, then the remaining DRV5055.
static void slot_label(int slot, char *buf, size_t buflen)
{
    if (slot < 9)
    {
        snprintf(buf, buflen, "%d (%s,%s)", slot,
                 state_name((magnet_state_t)(slot / 3)),
                 state_name((magnet_state_t)(slot % 3)));
        return;
    }
    // Learned entry: name it by the axis its stored direction leans on most, and
    // by that component's sign. Neither is what the match uses -- it compares the
    // whole vector -- but together they tell two entries apart at a glance, and
    // since a reversed magnet is now its own cartridge, the sign is half of what
    // distinguishes a pair of them.
    tmag_vec_t ref;
    if (!fp_load(slot, &ref))
    {
        snprintf(buf, buflen, "%d (empty)", slot);
        return;
    }

    float a[3] = {ref.x, ref.y, ref.z};
    int dom = 0;
    for (int i = 1; i < 3; ++i)
        if (fabsf(a[i]) > fabsf(a[dom]))
            dom = i;

    snprintf(buf, buflen, "%d (%c%c %.0f)", slot, "XYZ"[dom],
             a[dom] < 0 ? '-' : '+', tmag5273_magnitude(&ref));
}

static void slot_key(int slot, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "Slot%d", slot);
}

// Emulator that owns the slot's ROM, stored alongside the path. Without it the
// extension has to be guessed, which can't work for .zip and can't tell a .gb
// in the gb folder from one in the gbc folder.
static void slot_app_key(int slot, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "Slot%dApp", slot);
}

// The game on a slot, named the way the owner picked it. Empty string when the
// slot has nothing on it. Exists so the owner-facing dialogs can talk about
// "Tetris" where the diagnostics talk about slot 10.
static void slot_game_name(int slot, char *buf, size_t buflen)
{
    char key[MAGNET_KEY_LEN];
    slot_key(slot, key, sizeof(key));
    char *path = rg_settings_get_string(MAGNET_NS, key, NULL);
    if (path && *path)
        snprintf(buf, buflen, "%s", strcmp(path, MAGNET_SHOWCASE_TOKEN) == 0
                                        ? "Showcase"
                                        : rg_basename(path));
    else
        buf[0] = '\0';
    free(path);
}

static retro_app_t *app_for_slot(int slot, const char *path)
{
    char key[MAGNET_KEY_LEN];
    slot_app_key(slot, key, sizeof(key));
    char *name = rg_settings_get_string(MAGNET_NS, key, NULL);
    retro_app_t *app = (name && *name) ? application_find(name) : NULL;
    free(name);
    if (app)
        return app;

    // Mapping saved before the app name was stored: fall back to the extension.
    const char *dot = strrchr(path, '.');
    if (!dot)
        return NULL;
    if (strcasecmp(dot, ".gb") == 0)  return application_find("gb");
    if (strcasecmp(dot, ".gbc") == 0) return application_find("gbc");
    return NULL;
}

void magnet_launch_check_and_boot(void)
{
    if (!rg_settings_get_number(MAGNET_NS, MAGNET_KEY_ENABLE, 0))
        return;

    magnet_launch_init();

    magnet_reading_t r;
    int slot = magnet_read_stable(&r);

    if (tmag_present)
    {
        RG_LOGI("Magnet: raw=(%.0f,%.0f,%.0f) net=(%.0f,%.0f,%.0f) |net|=%.0f "
                "thr=%d match=%.3f next=%.3f flip=%d/%.3f slot=%d",
                r.field.x, r.field.y, r.field.z,
                r.net.x, r.net.y, r.net.z, tmag5273_magnitude(&r.net),
                magnet_field_threshold(), r.match, r.runner_up,
                r.flipped, r.flip_match, slot);
        if (!zero_known)
            RG_LOGW("Magnet: this console has never been calibrated. Readings "
                    "still carry the board's own field, so an empty slot can "
                    "match a pairing. Magnet cartridges > Calibrate.");
        if (slot == 0 && r.flip_match >= MAGNET_FP_MATCH)
            RG_LOGW("Magnet: no match, but slot %d matches this reading reversed "
                    "-- cartridge magnet is in the other way round",
                    r.flipped);
    }
    else
        RG_LOGI("Magnet: raw=(%d,%d) center=(%d,%d) band=(%d,%d) state=(%s,%s) slot=%d",
                r.raw[0], r.raw[1], magnet_get_center(0), magnet_get_center(1),
                magnet_get_band(0), magnet_get_band(1),
                state_name(r.adc[0]), state_name(r.adc[1]), slot);

    if (slot < 0)
    {
        RG_LOGW("Magnet: reading not stable, skipping auto-launch");
        return;
    }
    if (slot == 0)
        return;

    char key[MAGNET_KEY_LEN];
    slot_key(slot, key, sizeof(key));
    char *path = rg_settings_get_string(MAGNET_NS, key, NULL);
    if (!path || !*path) {
        free(path);
        return;
    }

    // "Showcase cartridge": instead of one ROM, start showcase mode. We just
    // flag it active here; the launcher's DisplayActive auto-launch (which runs
    // right after this returns) builds the list and launches the first game.
    if (strcmp(path, MAGNET_SHOWCASE_TOKEN) == 0) {
        free(path);
        int mode = gui.display_mode;
        if (mode == DISPLAY_MODE_OFF)
            mode = DISPLAY_MODE_SHUFFLE;
        RG_LOGI("Magnet slot %d -> showcase (mode %d)", slot, mode);
        rg_settings_set_number(NS_GLOBAL, "DisplayActive", 1);
        rg_settings_set_number(NS_GLOBAL, "DisplayFilter", mode);
        rg_settings_set_number(NS_GLOBAL, "DisplayIndex", 0);
        rg_settings_set_number(NS_GLOBAL, "DisplaySeed", (uint32_t)rg_system_timer());
        rg_settings_set_number(NS_GLOBAL, "DisplayElapsedSec", 0);
        rg_settings_commit();
        return;
    }

    if (!rg_storage_exists(path)) {
        RG_LOGW("Magnet slot %d: file not found: %s", slot, path);
        free(path);
        return;
    }

    retro_app_t *app = app_for_slot(slot, path);
    if (!app) {
        RG_LOGW("Magnet slot %d: no app for %s", slot, path);
        free(path);
        return;
    }

    RG_LOGI("Magnet auto-launch: slot %d -> %s", slot, path);
    rg_system_switch_app(app->partition, app->short_name, path, RG_BOOT_ONCE);
    // unreachable
}

// ----- Calibration -----

// Measures both sensors with no cartridge present and stores each one's idle
// point. Everything else is expressed relative to these, so this is the first
// thing to run on a new board.
static bool magnet_calibrate(void)
{
    if (magnet_adc_count() == 0)
        return false; // TMAG board: absolute readings, nothing to calibrate

    if (!rg_gui_confirm("Calibrate", "Remove the cartridge,\nthen confirm.", true))
        return false;

    magnet_launch_init();

    const int sensors = magnet_adc_count();
    int sum[MAGNET_SENSORS] = {0, 0};
    int count[MAGNET_SENSORS] = {0, 0};

    // Interleaved so any slow drift (supply, temperature) hits both equally.
    for (int i = 0; i < MAGNET_CAL_ROUNDS; ++i)
    {
        for (int s = 0; s < sensors; ++s)
        {
            int value = magnet_read_avg(s);
            if (value >= 0)
            {
                sum[s] += value;
                count[s]++;
            }
        }
        rg_task_delay(20);
    }

    for (int s = 0; s < sensors; ++s)
    {
        if (!count[s])
        {
            rg_gui_alert("Calibration failed", "No usable ADC readings.\nCheck the sensor wiring.");
            return false;
        }
    }

    int center[MAGNET_SENSORS] = {MAGNET_CENTER_DEFAULT, MAGNET_CENTER_DEFAULT};
    char msg[256];
    bool suspicious = false;

    for (int s = 0; s < sensors; ++s)
    {
        center[s] = sum[s] / count[s];
        if (center[s] < MAGNET_CENTER_MIN || center[s] > MAGNET_CENTER_MAX)
            suspicious = true;
    }

    if (suspicious)
    {
        int len = snprintf(msg, sizeof(msg),
                           "Idle readings are not near\nmid-scale (expected ~%d):\n",
                           MAGNET_CENTER_DEFAULT);
        for (int s = 0; s < sensors && len < (int)sizeof(msg); ++s)
            len += snprintf(msg + len, sizeof(msg) - len, "  %c = %d\n",
                            s == 0 ? 'L' : 'R', center[s]);
        snprintf(msg + len, sizeof(msg) - len,
                 "\nCheck the 3.3V supply and\nwiring. Save anyway?");
        if (!rg_gui_confirm("Suspicious readings", msg, false))
            return false;
    }

    for (int s = 0; s < sensors; ++s)
        rg_settings_set_number(MAGNET_NS, magnet_key_center[s], center[s]);
    rg_settings_commit();

    int len = snprintf(msg, sizeof(msg), "Idle points stored:\n");
    for (int s = 0; s < sensors && len < (int)sizeof(msg); ++s)
        len += snprintf(msg + len, sizeof(msg) - len, "  %c = %d\n",
                        s == 0 ? 'L' : 'R', center[s]);
    rg_gui_alert("Calibrated", msg);
    return true;
}

// ----- Options menu -----

static rg_gui_event_t auto_launch_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int enabled = rg_settings_get_number(MAGNET_NS, MAGNET_KEY_ENABLE, 0);
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER) {
        enabled = !enabled;
        rg_settings_set_number(MAGNET_NS, MAGNET_KEY_ENABLE, enabled);
    }
    strcpy(option->value, enabled ? "On" : "Off");
    return RG_DIALOG_VOID;
}

// Deleting one cartridge: its ROM, the app that ROM belongs to, and the field
// direction that identifies it.
static void magnet_forget(int slot)
{
    char key[MAGNET_KEY_LEN];
    slot_key(slot, key, sizeof(key));
    rg_settings_delete(MAGNET_NS, key);
    slot_app_key(slot, key, sizeof(key));
    rg_settings_delete(MAGNET_NS, key);
    fp_key(slot, key, sizeof(key));
    rg_settings_delete(MAGNET_NS, key);
    rg_settings_commit();
}

static rg_gui_event_t show_readings_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    magnet_launch_init();

    const int sensors = magnet_adc_count();
    int band[MAGNET_SENSORS] = {magnet_get_band(0), magnet_get_band(1)};
    int threshold = magnet_field_threshold();

    char title[48], val_center[24], val_thr[16], val_match[32];
    char val_s[MAGNET_SENSORS][32], val_b[MAGNET_SENSORS][16];
    char val_axis[3][28], val_field[24], val_zero[24];

    // Which rows exist depends on what is fitted: with a TMAG5273 there is no
    // right-hand DRV5055 to show and the three field axes take its place. So the
    // list is built here and the row indices fall out of it, rather than being
    // hardcoded. Worst case: 3 axes + strength + 1 sensor + idle + threshold +
    // 1 band + calibrate + reset + terminator = 11.
    rg_gui_option_t opts[14];
    static const char *const axis_name[3] = {"Field X", "Field Y", "Field Z"};
    int row_band[MAGNET_SENSORS] = {-1, -1};
    int row_thr = -1;
    int rows = 0;

    if (tmag_present)
    {
        for (int a = 0; a < 3; ++a)
            opts[rows++] = (rg_gui_option_t){0, axis_name[a], val_axis[a], RG_DIALOG_FLAG_MESSAGE, NULL};
        opts[rows++] = (rg_gui_option_t){0, "Strength", val_field, RG_DIALOG_FLAG_MESSAGE, NULL};
        opts[rows++] = (rg_gui_option_t){0, "Zero", val_zero, RG_DIALOG_FLAG_MESSAGE, NULL};
        opts[rows++] = (rg_gui_option_t){0, "Match", val_match, RG_DIALOG_FLAG_MESSAGE, NULL};
    }
    for (int s = 0; s < sensors; ++s)
        opts[rows++] = (rg_gui_option_t){0, magnet_sensor_name[s], val_s[s], RG_DIALOG_FLAG_MESSAGE, NULL};
    if (sensors > 0)
        opts[rows++] = (rg_gui_option_t){0, "Idle points", val_center, RG_DIALOG_FLAG_MESSAGE, NULL};

    const int sel_first = rows;
    if (tmag_present)
    {
        row_thr = rows;
        opts[rows++] = (rg_gui_option_t){0, "Field thresh.", val_thr, RG_DIALOG_FLAG_NORMAL, NULL};
    }
    for (int s = 0; s < sensors; ++s)
    {
        row_band[s] = rows;
        opts[rows++] = (rg_gui_option_t){0, s == 0 ? "Sensor L band" : "Sensor R band",
                                         val_b[s], RG_DIALOG_FLAG_NORMAL, NULL};
    }
    int sel_cal = -1;
    if (sensors > 0)
    {
        sel_cal = rows;
        opts[rows++] = (rg_gui_option_t){0, "Calibrate (empty)", NULL, RG_DIALOG_FLAG_NORMAL, NULL};
    }
    const int sel_reset = rows;
    opts[rows++] = (rg_gui_option_t){0, "Reset defaults", NULL, RG_DIALOG_FLAG_NORMAL, NULL};
    opts[rows] = (rg_gui_option_t)RG_DIALOG_END;

    int sel = sel_first;
    uint32_t prev = 0;
    int64_t last_input_ts = 0;
    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);

    while (true) {
        magnet_reading_t r;
        int slot = magnet_read(&r);
        int center[MAGNET_SENSORS] = {magnet_get_center(0), magnet_get_center(1)};

        if (tmag_present)
        {
            // The axes show the zero-corrected vector, because that is what the
            // matcher sees. The raw total sits on the "Zero" row beside the
            // stored offset, where the two can be compared at a glance -- if
            // those are the same number, the slot is empty.
            if (r.field_valid)
            {
                const float axis[3] = {r.net.x, r.net.y, r.net.z};
                for (int a = 0; a < 3; ++a)
                    snprintf(val_axis[a], sizeof(val_axis[a]), "%+7.0f uT", axis[a]);
                snprintf(val_field, sizeof(val_field), "%7.0f uT",
                         tmag5273_magnitude(&r.net));
                if (zero_known)
                    snprintf(val_zero, sizeof(val_zero), "%.0f (raw %.0f)",
                             tmag5273_magnitude(&zero_vec),
                             tmag5273_magnitude(&r.field));
                else
                    snprintf(val_zero, sizeof(val_zero), "NOT SET (raw %.0f)",
                             tmag5273_magnitude(&r.field));
            }
            else
            {
                for (int a = 0; a < 3; ++a)
                    snprintf(val_axis[a], sizeof(val_axis[a]), "--");
                snprintf(val_field, sizeof(val_field), "read failed");
                snprintf(val_zero, sizeof(val_zero), "--");
            }

            // The score is what to watch while the raw numbers wobble: it is the
            // quantity the decision is actually made on.
            if (slot > 0)
                snprintf(val_match, sizeof(val_match), "%d: %.3f  next %.2f",
                         slot, r.match, r.runner_up);
            else if (r.match >= MAGNET_FP_MATCH)
                // Cleared the score but not the margin: two entries look alike,
                // which is a different problem from not recognising anything and
                // wants a different fix (move a magnet, not adjust a threshold).
                snprintf(val_match, sizeof(val_match), "%d? %.3f vs %.3f",
                         r.nearest, r.match, r.runner_up);
            else if (r.flip_match >= MAGNET_FP_MATCH)
                snprintf(val_match, sizeof(val_match), "rev of %d (%.2f)",
                         r.flipped, r.flip_match);
            else if (r.match > 0.0f)
                snprintf(val_match, sizeof(val_match), "none %.3f (<%.2f)",
                         r.match, MAGNET_FP_MATCH);
            else
                snprintf(val_match, sizeof(val_match), "-");

            snprintf(val_thr, sizeof(val_thr), "%d uT", threshold);
        }

        // Second column is the distance from that sensor's own idle point: this
        // is the number the band is compared against, so it's what you tune on.
        snprintf(title, sizeof(title), "Readings (slot %d)", slot);
        for (int s = 0; s < sensors; ++s)
        {
            snprintf(val_s[s], sizeof(val_s[s]), "%4d %+5d  %s",
                     r.raw[s], r.raw[s] - center[s], state_name(r.adc[s]));
            snprintf(val_b[s], sizeof(val_b[s]), "%d", band[s]);
        }
        if (sensors > 1)
            snprintf(val_center, sizeof(val_center), "%d / %d", center[0], center[1]);
        else if (sensors > 0)
            snprintf(val_center, sizeof(val_center), "%d", center[0]);

        rg_gui_draw_status_bars();
        rg_gui_draw_dialog(title, opts, sel);

        uint32_t now = rg_input_read_gamepad();
        uint32_t old = (rg_system_timer() - last_input_ts > 300000) ? 0 : prev;
        prev = now;

        if (now != old) {
            if (now & (RG_KEY_B | RG_KEY_OPTION | RG_KEY_MENU))
                break;

            if (now & RG_KEY_UP)
                sel = (sel <= sel_first) ? sel_reset : sel - 1;
            if (now & RG_KEY_DOWN)
                sel = (sel >= sel_reset) ? sel_first : sel + 1;

            if (sel == row_thr && (now & (RG_KEY_LEFT | RG_KEY_RIGHT))) {
                threshold += (now & RG_KEY_RIGHT) ? MAGNET_FIELD_STEP : -MAGNET_FIELD_STEP;
                if (threshold < MAGNET_FIELD_MIN) threshold = MAGNET_FIELD_MIN;
                if (threshold > MAGNET_FIELD_MAX) threshold = MAGNET_FIELD_MAX;
                rg_settings_set_number(MAGNET_NS, MAGNET_KEY_FIELD, threshold);
            }

            int edit = (sel == row_band[0]) ? 0 : (sel == row_band[1]) ? 1 : -1;
            if (edit >= 0) {
                if (now & RG_KEY_LEFT)  band[edit] -= MAGNET_BAND_STEP;
                if (now & RG_KEY_RIGHT) band[edit] += MAGNET_BAND_STEP;
                if (band[edit] < MAGNET_BAND_MIN) band[edit] = MAGNET_BAND_MIN;
                if (band[edit] > MAGNET_BAND_MAX) band[edit] = MAGNET_BAND_MAX;
                if (now & (RG_KEY_LEFT | RG_KEY_RIGHT))
                    rg_settings_set_number(MAGNET_NS, magnet_key_band[edit], band[edit]);
            }

            if ((now & RG_KEY_A) && sel_cal >= 0 && sel == sel_cal) {
                magnet_calibrate();
                rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
                prev = 0;
            }

            if ((now & RG_KEY_A) && sel == sel_reset) {
                char prompt[160];
                if (sensors > 0)
                    snprintf(prompt, sizeof(prompt),
                             "Restore defaults?\n  DRV idle = %d\n  DRV band = %d",
                             MAGNET_CENTER_DEFAULT, MAGNET_BAND_DEFAULT);
                else
                    snprintf(prompt, sizeof(prompt),
                             "Restore defaults?\n  Field thresh. = %d uT", MAGNET_FIELD_DEFAULT);
                if (rg_gui_confirm("Reset", prompt, false)) {
                    for (int s = 0; s < sensors; ++s) {
                        band[s] = MAGNET_BAND_DEFAULT;
                        rg_settings_set_number(MAGNET_NS, magnet_key_center[s], MAGNET_CENTER_DEFAULT);
                        rg_settings_set_number(MAGNET_NS, magnet_key_band[s], MAGNET_BAND_DEFAULT);
                    }
                    threshold = MAGNET_FIELD_DEFAULT;
                    rg_settings_set_number(MAGNET_NS, MAGNET_KEY_FIELD, threshold);
                }
                rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
                prev = 0;
            }

            last_input_ts = rg_system_timer();
        }

        rg_task_delay(80);
        rg_system_tick(0);
    }

    rg_settings_commit();
    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
    rg_display_force_redraw();
    return RG_DIALOG_VOID;
}

typedef struct {
    char path[RG_PATH_MAX + 1];
    char app_name[16];
    bool showcase;
} magnet_pick_t;

static bool pick_rom(magnet_pick_t *out)
{
    // Magnet mode is a Game Boy feature only: no GBA, no other systems.
    static const char *const short_names[] = {"gb", "gbc"};
    retro_app_t *apps[RG_COUNT(short_names)] = {0};

    size_t total = 0;
    for (size_t a = 0; a < RG_COUNT(short_names); ++a) {
        apps[a] = application_find(short_names[a]);
        if (!apps[a])
            continue;
        application_init(apps[a]);
        for (size_t i = 0; i < apps[a]->files_count; ++i)
            if (apps[a]->files[i].type == RETRO_TYPE_FILE)
                total++;
    }

    if (total == 0) {
        rg_gui_alert("No ROMs", "No GB or GBC ROMs found\nin storage.");
        return false;
    }

    rg_gui_option_t *options = calloc(total + 2, sizeof(rg_gui_option_t));
    const retro_file_t **files = calloc(total, sizeof(retro_file_t *));
    if (!options || !files) {
        free(options);
        free(files);
        return false;
    }

    int n = 0;
    // First entry: assign this cartridge to Showcase mode instead of one ROM.
    options[n++] = (rg_gui_option_t){(intptr_t)total, "* Showcase", NULL, RG_DIALOG_FLAG_NORMAL, NULL};

    size_t idx = 0;
    for (size_t a = 0; a < RG_COUNT(short_names); ++a) {
        if (!apps[a])
            continue;
        for (size_t i = 0; i < apps[a]->files_count; ++i) {
            if (apps[a]->files[i].type != RETRO_TYPE_FILE) continue;
            files[idx] = &apps[a]->files[i];
            options[n++] = (rg_gui_option_t){(intptr_t)idx, apps[a]->files[i].name,
                                            apps[a]->short_name, RG_DIALOG_FLAG_NORMAL, NULL};
            idx++;
        }
    }
    options[n] = (rg_gui_option_t)RG_DIALOG_END;

    intptr_t sel = rg_gui_dialog("Pick ROM", options, 0);
    free(options);

    bool picked = false;
    if (sel == (intptr_t)total) {
        *out = (magnet_pick_t){.showcase = true};
        picked = true;
    } else if (sel >= 0 && (size_t)sel < total) {
        const retro_file_t *file = files[sel];
        *out = (magnet_pick_t){.showcase = false};
        snprintf(out->path, sizeof(out->path), "%s/%s", file->folder, file->name);
        if (file->app)
            snprintf(out->app_name, sizeof(out->app_name), "%s", file->app->short_name);
        picked = true;
    }
    free(files);
    return picked;
}

static rg_gui_event_t pair_current_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    magnet_launch_init();

    // Pairing before the console has been calibrated stores the board's own
    // field along with the cartridge's, and here that offset is several times
    // the signal. Every pairing made that way has to be redone later, so it is
    // refused -- but offered as something to fix now, since this is the moment
    // the owner finds out and they already have the console in their hands.
    if (tmag_present && !zero_known)
    {
        if (!rg_gui_confirm("Calibrate first",
                            "This console has not been\ncalibrated yet, so it\n"
                            "cannot tell a cartridge\nfrom its own magnets.\n\n"
                            "Do that now?", true))
            return RG_DIALOG_VOID;
        if (!magnet_take_zero())
            return RG_DIALOG_VOID;
        // The cartridge came out for the measurement, so the reading taken below
        // would be of an empty slot. Send them round once more rather than pair
        // whatever is in there now.
        rg_gui_alert("Ready",
                     "Put the cartridge back in,\nthen choose\n\"Pair cartridge\" again.");
        return RG_DIALOG_VOID;
    }

    magnet_reading_t r;
    int slot = magnet_read(&r);

    // On the TMAG board a cartridge that has never been paired reads as slot 0,
    // because there is nothing stored for it to match -- which is precisely the
    // case this menu exists for. So presence is decided on field strength here,
    // not on whether the cartridge was recognised. (The DRV board computes its
    // slot from the sensors, so there slot 0 really does mean "nothing".)
    bool present = tmag_present
                       ? (r.field_valid &&
                          tmag5273_magnitude(&r.net) >= (float)magnet_field_threshold())
                       : (slot != 0);

    if (!present) {
        char msg[224];
        if (tmag_present)
            snprintf(msg, sizeof(msg),
                     "No magnet found.\n\nCheck that the cartridge has\n"
                     "a magnet in it and that it\nis pushed all the way in,\n"
                     "then try again.");
        else
            snprintf(msg, sizeof(msg),
                     "Both sensors read idle:\n  L = %d (%+d)\n  R = %d (%+d)\n\n"
                     "Insert a cartridge, or run\nCalibrate if this looks wrong.",
                     r.raw[0], r.raw[0] - magnet_get_center(0),
                     r.raw[1], r.raw[1] - magnet_get_center(1));
        rg_gui_alert("No cartridge", msg);
        return RG_DIALOG_VOID;
    }

    char prompt[320], game[64];
    if (tmag_present)
    {
        // An already-matching cartridge is re-pointed at a new ROM rather than
        // given a second entry, otherwise re-pairing the same one silently fills
        // up the table with duplicates that all match each other.
        bool reuse = (slot > 0);
        if (!reuse)
        {
            slot = fp_first_free();
            if (slot == 0)
            {
                rg_gui_alert("No room",
                         "There is no room for\nanother cartridge.\n\n"
                         "Remove one under\n\"My cartridges\" first.");
                return RG_DIALOG_VOID;
            }
        }
        // A new cartridge sitting close to an existing one is the failure worth
        // catching while the magnet can still be moved -- afterwards the two are
        // simply unreliable and nothing in the UI explains why. So ask, plainly,
        // rather than printing a number the owner has no way to interpret. Now
        // that polarity counts, turning the magnet over is a second way out of
        // it and worth offering alongside moving the magnet.
        if (!reuse && r.match > 0.85f && r.nearest > 0)
        {
            slot_game_name(r.nearest, game, sizeof(game));
            char clash[256];
            snprintf(clash, sizeof(clash),
                     "This magnet position is\nalready used for:\n\n  %s\n\n"
                     "Move the magnet somewhere\nelse, or turn it over.\n\n"
                     "Continue anyway?",
                     *game ? game : "another cartridge");
            if (!rg_gui_confirm("Position taken", clash, false))
                return RG_DIALOG_VOID;
        }

        // No slot number and no field values on this screen. The owner needs one
        // thing from it: is this cartridge new, or does it already have a game.
        // Everything measurable lives under Advanced, where somebody diagnosing
        // goes looking and an owner does not -- and the numbering there starts at
        // 9, which would only ever raise a question this dialog cannot answer.
        if (reuse)
        {
            slot_game_name(slot, game, sizeof(game));
            if (*game)
                snprintf(prompt, sizeof(prompt),
                         "This cartridge already\nstarts:\n\n  %s\n\n"
                         "Pick a different game?", game);
            else
                snprintf(prompt, sizeof(prompt),
                         "This cartridge is known,\nbut has no game on it.\n\n"
                         "Pick one?");
        }
        else if (r.flip_match >= MAGNET_FP_MATCH && r.flipped > 0)
        {
            // Turning a magnet over makes a second, separate cartridge, so this
            // is not a clash. Naming the one it is the reverse of is what
            // separates a deliberate choice from a mistake nobody notices.
            slot_game_name(r.flipped, game, sizeof(game));
            snprintf(prompt, sizeof(prompt),
                     "This position is free. It is\n%s with the\n"
                     "magnet the other way round.\n\nPick a game for it?",
                     *game ? game : "another cartridge");
        }
        else
            snprintf(prompt, sizeof(prompt),
                     "This magnet position is\nfree.\n\nPick a game for it?");
    }
    else
    {
        // Same words as the TMAG board. The measurement behind them is a
        // different one -- here the slot is computed from two N/S readings
        // rather than learned -- but what the owner has to decide is identical,
        // and the sensor states belong on the readings screen, not here.
        slot_game_name(slot, game, sizeof(game));
        if (*game)
            snprintf(prompt, sizeof(prompt),
                     "This cartridge already\nstarts:\n\n  %s\n\n"
                     "Pick a different game?", game);
        else
            snprintf(prompt, sizeof(prompt),
                     "This magnet position is\nfree.\n\nPick a game for it?");
    }
    if (!rg_gui_confirm("Pair cartridge", prompt, true))
        return RG_DIALOG_VOID;

    magnet_pick_t pick;
    if (!pick_rom(&pick))
        return RG_DIALOG_VOID;

    // Stored after the ROM is picked, so a cancelled pick leaves no orphan entry.
    if (tmag_present)
        fp_store(slot, &r.net);

    char key[MAGNET_KEY_LEN];
    slot_key(slot, key, sizeof(key));
    rg_settings_set_string(MAGNET_NS, key, pick.showcase ? MAGNET_SHOWCASE_TOKEN : pick.path);
    slot_app_key(slot, key, sizeof(key));
    if (pick.showcase || !pick.app_name[0])
        rg_settings_delete(MAGNET_NS, key);
    else
        rg_settings_set_string(MAGNET_NS, key, pick.app_name);
    rg_settings_commit();

    // Nobody pairs a cartridge and then wants nothing to happen when they switch
    // the device on, so the first pairing switches auto-start on by itself. It
    // stays a toggle for anyone who wants it off again.
    bool armed = false;
    if (!rg_settings_get_number(MAGNET_NS, MAGNET_KEY_ENABLE, 0))
    {
        rg_settings_set_number(MAGNET_NS, MAGNET_KEY_ENABLE, 1);
        rg_settings_commit();
        armed = true;
    }

    char done[256];
    snprintf(done, sizeof(done), "%s is now on this\ncartridge.%s",
             pick.showcase ? "Showcase" : rg_basename(pick.path),
             armed ? "\n\nAuto-start switched on." : "");
    rg_gui_alert("Saved", done);
    return RG_DIALOG_VOID;
}

// The paired-cartridge list. `technical` picks between what the owner needs --
// a numbered cartridge and the game on it -- and what diagnosing needs, which is
// the stored direction. Selecting an entry offers to forget it either way.
static void magnet_list(bool technical)
{
    // 27 entries can exist but only a handful are ever paired, so list those.
    // The buffers are static because a full-size copy does not belong on the
    // launcher's stack.
    enum { MAX_SHOWN = 24 };
    static char labels[MAX_SHOWN][24];
    static char values[MAX_SHOWN][32];
    int slots[MAX_SHOWN] = {0};
    rg_gui_option_t opts[MAX_SHOWN + 2];

    int n = 0;
    for (int slot = 1; slot < MAGNET_SLOT_COUNT && n < MAX_SHOWN; ++slot) {
        char key[MAGNET_KEY_LEN];
        slot_key(slot, key, sizeof(key));
        char *path = rg_settings_get_string(MAGNET_NS, key, NULL);
        if (!path || !*path) {
            free(path);
            continue;
        }

        const char *name = (strcmp(path, MAGNET_SHOWCASE_TOKEN) == 0) ? "Showcase"
                                                                      : rg_basename(path);
        if (technical)
            slot_label(slot, labels[n], sizeof(labels[n]));
        else
            snprintf(labels[n], sizeof(labels[n]), "Cartridge %d", n + 1);
        snprintf(values[n], sizeof(values[n]), "%s", name);
        free(path);

        slots[n] = slot;
        opts[n] = (rg_gui_option_t){slot, labels[n], values[n], RG_DIALOG_FLAG_NORMAL, NULL};
        n++;
    }

    if (n == 0) {
        rg_gui_alert("Cartridges", "No cartridges paired yet.\n\nUse \"Pair cartridge\" with\na cartridge inserted.");
        return;
    }

    opts[n] = (rg_gui_option_t)RG_DIALOG_END;

    intptr_t chosen = rg_gui_dialog(technical ? "Slot details" : "My cartridges", opts, 0);
    if (chosen == RG_DIALOG_CANCELLED)
        return;

    for (int i = 0; i < n; ++i) {
        if (slots[i] != (int)chosen)
            continue;
        char msg[128];
        snprintf(msg, sizeof(msg), "Forget this cartridge?\n\n  %s", values[i]);
        if (rg_gui_confirm("Remove", msg, false)) {
            magnet_forget(slots[i]);
            rg_gui_alert("Removed", "The cartridge is no longer\nlinked to a game.");
        }
        break;
    }
}

static rg_gui_event_t my_cartridges_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
        magnet_list(false);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t slot_details_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
        magnet_list(true);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t clear_all_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    // Asking "are you sure" about nothing is worse than saying there is nothing
    // to be sure about, and it is the answer somebody wants when they came here
    // expecting a list.
    int paired = 0;
    for (int slot = 1; slot < MAGNET_SLOT_COUNT; ++slot) {
        char name[64];
        slot_game_name(slot, name, sizeof(name));
        if (*name)
            paired++;
    }
    if (!paired) {
        rg_gui_alert("Forget all", "No cartridges are set up.");
        return RG_DIALOG_VOID;
    }

    char msg[192];
    snprintf(msg, sizeof(msg),
             "Forget %s cartridge%s?\n\nThe magnets stay in them,\n"
             "but none of them will\nstart a game any more.",
             paired == 1 ? "the" : "all", paired == 1 ? "" : "s");
    if (!rg_gui_confirm("Forget all", msg, false))
        return RG_DIALOG_VOID;
    // The calibration survives. It describes the console, not the pairings, and
    // having to redo it after clearing a set of mappings would be a trap rather
    // than a safeguard.
    for (int slot = 1; slot < MAGNET_SLOT_COUNT; ++slot) {
        char key[MAGNET_KEY_LEN];
        slot_key(slot, key, sizeof(key));
        rg_settings_delete(MAGNET_NS, key);
        slot_app_key(slot, key, sizeof(key));
        rg_settings_delete(MAGNET_NS, key);
        fp_key(slot, key, sizeof(key));
        rg_settings_delete(MAGNET_NS, key);
    }
    rg_settings_commit();
    rg_gui_alert("Done", "The cartridges no longer\nstart a game.\n\n"
                         "Pair them again whenever\nyou like.");
    return RG_DIALOG_VOID;
}

// ----- Zero measurement -----

// Measures the field with the slot empty and stores it. Everything the matcher
// does afterwards is relative to this vector, so it is the first thing to run on
// a new board -- on this hardware it is the difference between a working system
// and one that launches a game at an empty slot.
//
// A production step, not something an owner ever sees: the offset is a property
// of where the speaker sits relative to the sensor, so it is measured once per
// console and then holds. The owner's job stays "put a magnet in the cartridge
// and pair it to a ROM".
static bool magnet_take_zero(void)
{
    if (!tmag_present)
        return false;

    if (!rg_gui_confirm("Calibrate",
                        "Take the cartridge out of\nthe slot and lay the\n"
                        "console down flat.\n\nThis measures the console's\n"
                        "own magnetism so it can\ntell a cartridge from it.\n\n"
                        "Slot empty?", true))
        return false;

    magnet_launch_init();

    tmag_vec_t sum = {0.0f, 0.0f, 0.0f};
    int got = 0;
    for (int i = 0; i < MAGNET_ZERO_SAMPLES; ++i)
    {
        tmag_vec_t v;
        if (tmag5273_read(&v))
        {
            sum.x += v.x;
            sum.y += v.y;
            sum.z += v.z;
            got++;
        }
        rg_task_delay(2);
    }

    // Half is a generous floor -- it takes a bus failing intermittently to get
    // there -- but storing a zero built from three samples would be worse than
    // storing none, because nothing downstream would ever flag it.
    if (got < MAGNET_ZERO_SAMPLES / 2)
    {
        rg_gui_alert("Calibration failed",
                     "The magnet sensor did not\nrespond. Try again, and if\n"
                     "it keeps failing the console\nneeds service.");
        return false;
    }

    tmag_vec_t z = {sum.x / got, sum.y / got, sum.z / got};

    // Pairings made before any zero existed still have the offset baked in.
    // Subtracting today's offset is exactly the correction they never got --
    // accurate to the earth-field difference between the two poses, tens of uT
    // against an error of hundreds. Far better than making the owner re-pair.
    int rebased = 0;
    if ((int)rg_settings_get_number(MAGNET_NS, MAGNET_KEY_VER, 1) < MAGNET_VER_ZEROED)
    {
        for (int slot = MAGNET_FP_FIRST; slot < MAGNET_SLOT_COUNT; ++slot)
        {
            tmag_vec_t ref;
            if (!fp_load(slot, &ref))
                continue;
            ref.x -= z.x;
            ref.y -= z.y;
            ref.z -= z.z;
            fp_store(slot, &ref);
            rebased++;
        }
    }

    vec_save(MAGNET_KEY_ZERO, &z);
    rg_settings_set_number(MAGNET_NS, MAGNET_KEY_VER, MAGNET_VER_ZEROED);
    rg_settings_commit();
    zero_refresh();

    // The owner gets a plain first line and one number they could repeat back
    // over the phone; the vector itself stays out of it and lives on the
    // readings screen, where somebody diagnosing will look anyway.
    char msg[256];
    int len = snprintf(msg, sizeof(msg),
                       "Done.\n\nThe console's own field\nmeasures %.0f uT.\n",
                       tmag5273_magnitude(&z));
    if (rebased && len < (int)sizeof(msg))
        snprintf(msg + len, sizeof(msg) - len,
                 "\n%d cartridge%s already\npaired %s updated too.",
                 rebased, rebased == 1 ? "" : "s", rebased == 1 ? "was" : "were");
    rg_gui_alert("Calibrated", msg);
    return true;
}

// One entry, either board. On the TMAG board calibrating means measuring the
// console's own field; on the two-DRV5055 board it means finding each sensor's
// idle point. Different measurements, same thing to the owner -- "the console
// learns what empty looks like" -- so they do not get two menu items and a
// choice they have no way to make.
static rg_gui_event_t calibrate_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;
    if (tmag_present)
        magnet_take_zero();
    else
        magnet_calibrate();
    return RG_DIALOG_VOID;
}

// ----- Self test -----
//
// Two numbers decide whether magnet launch can be reliable at all, and neither
// is visible on the readings screen because both are spreads over time:
//
//   pose  -- the console tilted while the cartridge stays put. This is the earth
//            field (~50uT) turning in the sensor's frame, and it is irreducible:
//            a fixed offset cannot cancel a rotating one. It sets how strong the
//            cartridge magnet has to be.
//   mech  -- the cartridge pulled out and pushed back in the same pose. This is
//            play in the slot, and it sets how far apart two magnet positions
//            have to sit.
//
// The screen separates them by itself. Presence is decided per sample, so
// pulling the cartridge closes one insertion and pushing it in opens the next --
// no mode to select, no button to press while both hands hold a cartridge.

#define SELFTEST_MAX_INSERTS   16
// Samples an insertion needs before it counts, so the vector sweeping past the
// threshold on its way in does not register as an insertion of its own.
#define SELFTEST_MIN_SAMPLES   4

typedef struct {
    tmag_vec_t sum;
    int        n;
    float      mag_min, mag_max;
    float      ang_max; // widest angle any sample made with the running mean
} st_acc_t;

static void st_reset(st_acc_t *a)
{
    *a = (st_acc_t){{0.0f, 0.0f, 0.0f}, 0, 0.0f, 0.0f, 0.0f};
}

static tmag_vec_t st_mean(const st_acc_t *a)
{
    if (!a->n)
        return (tmag_vec_t){0.0f, 0.0f, 0.0f};
    return (tmag_vec_t){a->sum.x / a->n, a->sum.y / a->n, a->sum.z / a->n};
}

// Degrees between two vectors. Reported as an angle rather than as a cos because
// the spreads below are small numbers there and unreadable ones here: 0.999 and
// 0.995 are 2.6 and 5.7 degrees, and only one of those is a problem.
static float st_angle(const tmag_vec_t *a, const tmag_vec_t *b)
{
    float c = fp_similarity(a, b);
    if (c > 1.0f)
        c = 1.0f;
    if (c < -1.0f)
        c = -1.0f;
    return acosf(c) * 57.2957795f;
}

// Angle is taken against the mean so far rather than the final mean: a second
// pass is not possible in a live loop, and the early bias only ever makes the
// spread look slightly wider than it is, which is the safe direction to err.
static void st_add(st_acc_t *a, const tmag_vec_t *v)
{
    float mag = tmag5273_magnitude(v);
    if (a->n)
    {
        tmag_vec_t mean = st_mean(a);
        float ang = st_angle(v, &mean);
        if (ang > a->ang_max)
            a->ang_max = ang;
        if (mag < a->mag_min)
            a->mag_min = mag;
        if (mag > a->mag_max)
            a->mag_max = mag;
    }
    else
    {
        a->mag_min = a->mag_max = mag;
    }
    a->sum.x += v->x;
    a->sum.y += v->y;
    a->sum.z += v->z;
    a->n++;
}

static rg_gui_event_t self_test_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;
    if (!tmag_present)
    {
        rg_gui_alert("Self test", "Needs the TMAG5273.\nThis board has none.");
        return RG_DIALOG_VOID;
    }

    magnet_launch_init();

    // Static for the same reason the cartridge list is: a full set of insertion
    // vectors plus the row buffers has no business on the launcher's stack.
    static tmag_vec_t inserts[SELFTEST_MAX_INSERTS];
    static char val_live[28], val_amb[28], val_in[28], val_pose[28],
                val_mech[28], val_mag[28], val_match[30];
    int inserts_n = 0;

    st_acc_t cur, empty;
    st_reset(&cur);
    st_reset(&empty);
    bool inserted = false;

    rg_gui_option_t opts[] = {
        {0, "Net",      val_live,  RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, "Ambient",  val_amb,   RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, "In slot",  val_in,    RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, "Pose",     val_pose,  RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, "Mech",     val_mech,  RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, "Strength", val_mag,   RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, "Match",    val_match, RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, "Reset",    NULL,      RG_DIALOG_FLAG_NORMAL,  NULL},
        RG_DIALOG_END,
    };
    const int sel_reset = 7;

    snprintf(val_live, sizeof(val_live), "--");
    snprintf(val_amb, sizeof(val_amb), "--");
    snprintf(val_in, sizeof(val_in), "out");
    snprintf(val_pose, sizeof(val_pose), "--");
    snprintf(val_mech, sizeof(val_mech), "--");
    snprintf(val_mag, sizeof(val_mag), "--");
    snprintf(val_match, sizeof(val_match), "--");

    uint32_t prev = 0;
    int64_t last_input_ts = 0;
    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);

    while (true)
    {
        magnet_reading_t r;
        int slot = magnet_read(&r);

        if (r.field_valid)
        {
            float mag = tmag5273_magnitude(&r.net);
            bool now_in = mag >= (float)magnet_field_threshold();

            if (now_in)
            {
                if (!inserted)
                    st_reset(&cur);
                inserted = true;
                st_add(&cur, &r.net);
            }
            else
            {
                // Closing an insertion is what advances the between-insertion
                // figure, so a brief glitch above the threshold must not count
                // as one -- hence the sample floor.
                if (inserted && cur.n >= SELFTEST_MIN_SAMPLES &&
                    inserts_n < SELFTEST_MAX_INSERTS)
                    inserts[inserts_n++] = st_mean(&cur);
                inserted = false;
                // Raw, not net: with the slot empty this is what a zero
                // measurement would store, so it can be held against the stored
                // one directly.
                st_add(&empty, &r.field);
            }

            snprintf(val_live, sizeof(val_live), "%+5.0f%+5.0f%+5.0f",
                     r.net.x, r.net.y, r.net.z);

            if (empty.n)
            {
                tmag_vec_t amb = st_mean(&empty);
                tmag_vec_t drift = {amb.x - zero_vec.x, amb.y - zero_vec.y,
                                    amb.z - zero_vec.z};
                // Distance from the stored zero, not the angle between them:
                // what matters is how many uT of error the stored value injects.
                snprintf(val_amb, sizeof(val_amb), "%.0f  d%.0f  n%d",
                         tmag5273_magnitude(&amb),
                         zero_known ? tmag5273_magnitude(&drift) : 0.0f, empty.n);
            }

            if (inserted)
                snprintf(val_in, sizeof(val_in), "#%d  %.0f uT  n%d",
                         inserts_n + 1, mag, cur.n);
            else
                snprintf(val_in, sizeof(val_in), "out  %.0f uT", mag);

            if (cur.n > 1)
                snprintf(val_pose, sizeof(val_pose), "%.1f deg  %.0f..%.0f",
                         cur.ang_max, cur.mag_min, cur.mag_max);

            if (slot > 0)
                snprintf(val_match, sizeof(val_match), "%d: %.3f next %.3f",
                         slot, r.match, r.runner_up);
            else if (r.flip_match >= MAGNET_FP_MATCH)
                snprintf(val_match, sizeof(val_match), "rev of %d (%.2f)",
                         r.flipped, r.flip_match);
            else if (r.match != 0.0f)
                snprintf(val_match, sizeof(val_match), "no: %.3f next %.3f",
                         r.match, r.runner_up);
            else
                snprintf(val_match, sizeof(val_match), "-");
        }

        // Recomputed from the stored insertion means every frame rather than
        // accumulated, so this spread is exact against the final mean instead of
        // against whatever the mean happened to be at the time.
        if (inserts_n > 0)
        {
            tmag_vec_t m = {0.0f, 0.0f, 0.0f};
            for (int i = 0; i < inserts_n; ++i)
            {
                m.x += inserts[i].x;
                m.y += inserts[i].y;
                m.z += inserts[i].z;
            }
            m.x /= inserts_n;
            m.y /= inserts_n;
            m.z /= inserts_n;

            float worst = 0.0f;
            float lo = tmag5273_magnitude(&inserts[0]), hi = lo;
            for (int i = 0; i < inserts_n; ++i)
            {
                float a = st_angle(&inserts[i], &m);
                if (a > worst)
                    worst = a;
                float g = tmag5273_magnitude(&inserts[i]);
                if (g < lo) lo = g;
                if (g > hi) hi = g;
            }
            snprintf(val_mech, sizeof(val_mech), "%.1f deg  x%d", worst, inserts_n);
            snprintf(val_mag, sizeof(val_mag), "%.0f..%.0f  %.0f%%", lo, hi,
                     lo > 0.0f ? (hi - lo) * 100.0f / lo : 0.0f);
        }

        rg_gui_draw_status_bars();
        rg_gui_draw_dialog("Self test", opts, sel_reset);

        uint32_t now = rg_input_read_gamepad();
        uint32_t old = (rg_system_timer() - last_input_ts > 300000) ? 0 : prev;
        prev = now;

        if (now != old)
        {
            if (now & (RG_KEY_B | RG_KEY_OPTION | RG_KEY_MENU))
                break;
            if (now & RG_KEY_A)
            {
                inserts_n = 0;
                inserted = false;
                st_reset(&cur);
                st_reset(&empty);
            }
            last_input_ts = rg_system_timer();
        }

        rg_task_delay(40);
        rg_system_tick(0);
    }

    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
    rg_display_force_redraw();
    return RG_DIALOG_VOID;
}

// Everything that only matters when something is wrong, or that can destroy a
// set of pairings. Behind the service PIN via the "MagnetAdv" shop key.
rg_gui_event_t magnet_launch_advanced_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;
    const rg_gui_option_t opts[] = {
        // Calibrate lives inside "Show readings", where you can watch the
        // sensors while you do it. No second entry for it here.
        {0, "Show readings", NULL, RG_DIALOG_FLAG_NORMAL, &show_readings_cb},
        {0, "Self test",     NULL, RG_DIALOG_FLAG_NORMAL, &self_test_cb},
        {0, "Slot details",  NULL, RG_DIALOG_FLAG_NORMAL, &slot_details_cb},
        RG_DIALOG_END,
    };
    rg_gui_dialog("Advanced", opts, 0);
    return RG_DIALOG_VOID;
}

// What the owner sees: switch it on, add a cartridge, see what is on them.
// Anything diagnostic is one level down and normally hidden.
rg_gui_event_t magnet_launch_options_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    // Calibrate sits above Pair because that is the order they have to happen
    // in, and a console that has never been calibrated cannot pair at all.
    // Forget all goes last, below the list it empties.
    const rg_gui_option_t opts[] = {
        {0, "Auto-start",     "-",  RG_DIALOG_FLAG_NORMAL, &auto_launch_cb},
        {0, "Calibrate",      NULL, RG_DIALOG_FLAG_NORMAL, &calibrate_cb},
        {0, "Pair cartridge", NULL, RG_DIALOG_FLAG_NORMAL, &pair_current_cb},
        {0, "My cartridges",  NULL, RG_DIALOG_FLAG_NORMAL, &my_cartridges_cb},
        {0, "Forget all",     NULL, RG_DIALOG_FLAG_NORMAL, &clear_all_cb},
        RG_DIALOG_END,
    };
    rg_gui_dialog("Magnet cartridges", opts, 0);
    return RG_DIALOG_VOID;
}
