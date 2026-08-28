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
#define MAGNET_FIELD_MIN       50
#define MAGNET_FIELD_MAX       5000

#define MAGNET_NS              "magnet"
#define MAGNET_KEY_ENABLE      "Enable"
#define MAGNET_KEY_FIELD       "FieldUT"

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
    return tmag_present ? 0 : MAGNET_SENSORS;
}

void magnet_launch_init(void)
{
#ifdef ESP_PLATFORM
    if (initialized)
        return;

    tmag_present = tmag5273_init();

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
    tmag_vec_t     field;                // microtesla, only when field_valid
    bool           field_valid;
    int            nearest;              // closest entry, threshold or not
    float          match;                // its |cos| to this reading
    float          runner_up;            // the next closest entry's |cos|
} magnet_reading_t;

// Stored direction of one learned cartridge, as the microtesla vector that was
// measured when it was paired. Kept unnormalised so the saved value is readable
// and can be compared against a fresh reading by eye.
static void fp_key(int slot, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "Fp%d", slot);
}

static bool fp_load(int slot, tmag_vec_t *out)
{
    char key[MAGNET_KEY_LEN];
    fp_key(slot, key, sizeof(key));
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
        RG_LOGW("Magnet slot %d: cannot parse stored direction '%s'", slot, text);
    free(text);
    if (!ok)
        return false;

    out->x = v[0];
    out->y = v[1];
    out->z = v[2];
    return tmag5273_magnitude(out) > 0.0f;
}

static void fp_store(int slot, const tmag_vec_t *v)
{
    char key[MAGNET_KEY_LEN], text[48];
    fp_key(slot, key, sizeof(key));
    snprintf(text, sizeof(text), "%.0f,%.0f,%.0f", v->x, v->y, v->z);
    rg_settings_set_string(MAGNET_NS, key, text);
}

// |cos| between two field vectors. The absolute value is what makes a magnet
// turned over read as the same cartridge: reversing the magnet negates the whole
// vector, which flips the sign of the dot product and nothing else.
static float fp_similarity(const tmag_vec_t *a, const tmag_vec_t *b)
{
    float na = tmag5273_magnitude(a);
    float nb = tmag5273_magnitude(b);
    if (na <= 0.0f || nb <= 0.0f)
        return 0.0f;
    return fabsf((a->x * b->x + a->y * b->y + a->z * b->z) / (na * nb));
}

// Closest stored entry, whether or not it clears MAGNET_FP_MATCH -- the caller
// applies that. `score` gets its similarity and `second` the runner-up's, and
// the gap between those two is the real measure of how safe a match is: a 0.99
// that beats a 0.98 is a coin flip, a 0.99 that beats a 0.86 is not.
static int fp_nearest(const tmag_vec_t *v, float *score, float *second)
{
    int best_slot = 0;
    float best = 0.0f, next = 0.0f;

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
    }

    if (score)
        *score = best;
    if (second)
        *second = next;
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
    }

    if (!out->field_valid)
        return 0;

    // Strength decides whether a cartridge is there at all; direction decides
    // which one. Below the threshold the direction is just noise pointing
    // somewhere, so there is nothing to match against.
    if (tmag5273_magnitude(&out->field) < (float)magnet_field_threshold())
        return 0;

    out->nearest = fp_nearest(&out->field, &out->match, &out->runner_up);
    return (out->match >= MAGNET_FP_MATCH) ? out->nearest : 0;
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
    // Learned entry: name it by the axis its stored direction leans on most.
    // That is not what the match uses -- it compares the whole vector -- but it
    // is the one number that tells two entries apart at a glance.
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
        RG_LOGI("Magnet: field=(%.0f,%.0f,%.0f)uT |B|=%.0f thr=%d match=%.3f slot=%d",
                r.field.x, r.field.y, r.field.z, tmag5273_magnitude(&r.field),
                magnet_field_threshold(), r.match, slot);
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
    char val_axis[3][28], val_field[24];

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
            if (r.field_valid)
            {
                const float axis[3] = {r.field.x, r.field.y, r.field.z};
                for (int a = 0; a < 3; ++a)
                    snprintf(val_axis[a], sizeof(val_axis[a]), "%+7.0f uT", axis[a]);
                snprintf(val_field, sizeof(val_field), "%7.0f uT",
                         tmag5273_magnitude(&r.field));
            }
            else
            {
                for (int a = 0; a < 3; ++a)
                    snprintf(val_axis[a], sizeof(val_axis[a]), "--");
                snprintf(val_field, sizeof(val_field), "read failed");
            }

            // The score is what to watch while the raw numbers wobble: it is the
            // quantity the decision is actually made on.
            if (slot > 0)
                snprintf(val_match, sizeof(val_match), "%d: %.3f  next %.2f",
                         slot, r.match, r.runner_up);
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

    magnet_reading_t r;
    int slot = magnet_read(&r);

    // On the TMAG board a cartridge that has never been paired reads as slot 0,
    // because there is nothing stored for it to match -- which is precisely the
    // case this menu exists for. So presence is decided on field strength here,
    // not on whether the cartridge was recognised. (The DRV board computes its
    // slot from the sensors, so there slot 0 really does mean "nothing".)
    bool present = tmag_present
                       ? (r.field_valid &&
                          tmag5273_magnitude(&r.field) >= (float)magnet_field_threshold())
                       : (slot != 0);

    if (!present) {
        char msg[224];
        if (tmag_present)
            snprintf(msg, sizeof(msg),
                     "Put a magnet in one of the\nfour positions in the\n"
                     "cartridge, then insert it and\ntry again.\n\n(measured %.0f uT)",
                     tmag5273_magnitude(&r.field));
        else
            snprintf(msg, sizeof(msg),
                     "Both sensors read idle:\n  L = %d (%+d)\n  R = %d (%+d)\n\n"
                     "Insert a cartridge, or run\nCalibrate if this looks wrong.",
                     r.raw[0], r.raw[0] - magnet_get_center(0),
                     r.raw[1], r.raw[1] - magnet_get_center(1));
        rg_gui_alert("No cartridge", msg);
        return RG_DIALOG_VOID;
    }

    char prompt[320];
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
                rg_gui_alert("No free slots", "All cartridge slots are in\nuse. Clear one first.");
                return RG_DIALOG_VOID;
            }
        }
        // A new cartridge that already sits near an existing one is the failure
        // worth catching here rather than three cartridges later: say so while
        // the magnet can still be moved.
        char note[80] = "";
        if (reuse)
            snprintf(note, sizeof(note), "  Match: %.3f\n", r.match);
        else if (r.nearest > 0)
            snprintf(note, sizeof(note), "  Nearest: slot %d at %.2f%s\n",
                     r.nearest, r.match,
                     r.match > 0.85f ? "  (close!)" : "");

        // A new cartridge sitting close to an existing one is the failure worth
        // catching while the magnet can still be moved -- afterwards the two are
        // simply unreliable and nothing in the UI explains why. So ask, plainly,
        // rather than printing a number the owner has no way to interpret.
        if (!reuse && r.match > 0.85f && r.nearest > 0)
        {
            char other[RG_PATH_MAX + 1] = "another cartridge";
            char nkey[MAGNET_KEY_LEN];
            slot_key(r.nearest, nkey, sizeof(nkey));
            char *npath = rg_settings_get_string(MAGNET_NS, nkey, NULL);
            if (npath && *npath)
            {
                snprintf(other, sizeof(other), "%s",
                         strcmp(npath, MAGNET_SHOWCASE_TOKEN) == 0 ? "Showcase"
                                                                   : rg_basename(npath));
            }
            free(npath);

            char clash[256];
            snprintf(clash, sizeof(clash),
                     "This magnet position is\nalready used for:\n\n  %s\n\n"
                     "Use one of the other positions.\nContinue anyway?",
                     other);
            if (!rg_gui_confirm("Position taken", clash, false))
                return RG_DIALOG_VOID;
        }

        snprintf(prompt, sizeof(prompt),
                 "Cartridge %s:\n"
                 "  Field: %+.0f %+.0f %+.0f\n"
                 "  Strength: %.0f uT\n"
                 "%s"
                 "  Slot: %d%s\n\n"
                 "Pick a ROM to assign?",
                 reuse ? "recognised" : "is new",
                 r.field.x, r.field.y, r.field.z,
                 tmag5273_magnitude(&r.field), note, slot,
                 reuse ? " (re-assign)" : "");
    }
    else
        snprintf(prompt, sizeof(prompt),
                 "Cartridge detected:\n"
                 "  Sensor L: %s\n"
                 "  Sensor R: %s\n"
                 "  Slot: %d\n\n"
                 "Pick a ROM to assign?",
                 state_name(r.adc[0]), state_name(r.adc[1]), slot);
    if (!rg_gui_confirm("Pair cartridge", prompt, true))
        return RG_DIALOG_VOID;

    magnet_pick_t pick;
    if (!pick_rom(&pick))
        return RG_DIALOG_VOID;

    // Stored after the ROM is picked, so a cancelled pick leaves no orphan entry.
    if (tmag_present)
        fp_store(slot, &r.field);

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
    if (!rg_gui_confirm("Clear all", "Remove all magnet -> ROM mappings?", false))
        return RG_DIALOG_VOID;
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
    rg_gui_alert("Cleared", "All magnet mappings removed.");
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
        {0, "Slot details",  NULL, RG_DIALOG_FLAG_NORMAL, &slot_details_cb},
        {0, "Clear all",     NULL, RG_DIALOG_FLAG_NORMAL, &clear_all_cb},
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

    const rg_gui_option_t opts[] = {
        {0, "Auto-start",     "-",  RG_DIALOG_FLAG_NORMAL, &auto_launch_cb},
        {0, "Pair cartridge", NULL, RG_DIALOG_FLAG_NORMAL, &pair_current_cb},
        {0, "My cartridges",  NULL, RG_DIALOG_FLAG_NORMAL, &my_cartridges_cb},
        RG_DIALOG_END,
    };
    rg_gui_dialog("Magnet cartridges", opts, 0);
    return RG_DIALOG_VOID;
}
