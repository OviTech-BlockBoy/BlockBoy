#include "rg_system.h"
#include "rg_audio.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <esp_system.h> // esp_reset_reason()
#endif

extern const rg_audio_driver_t rg_audio_driver_dummy;
extern const rg_audio_driver_t rg_audio_driver_buzzer;
extern const rg_audio_driver_t rg_audio_driver_i2s;
extern const rg_audio_driver_t rg_audio_driver_sdl2;
extern const rg_audio_driver_t rg_audio_driver_uac_host;

static const rg_audio_sink_t sinks[] = {
    {&rg_audio_driver_dummy,  0, "Dummy"  },
#if RG_AUDIO_USE_INT_DAC
    {&rg_audio_driver_i2s,    0, "Speaker"},
#endif
#if RG_AUDIO_USE_EXT_DAC
    {&rg_audio_driver_i2s,    1, "Ext DAC"},
#endif
#if RG_AUDIO_USE_SDL2
    {&rg_audio_driver_sdl2,   0, "SDL2"   },
#endif
#if RG_AUDIO_USE_BUZZER_PIN
    {&rg_audio_driver_buzzer, 0, "Buzzer" },
#endif
#if RG_AUDIO_USE_UAC_HOST
    {&rg_audio_driver_uac_host, 0, "USB-C DAC"},
#endif
    // {rg_audio_driver_bt_a2dp, 0, "Bluetooth"},
};

#define ACQUIRE_DEVICE(timeout)                         \
    ({                                                  \
        bool lock = rg_mutex_take(audio.lock, timeout); \
        if (!lock)                                      \
            RG_LOGE("Failed to acquire lock!\n");       \
        lock;                                           \
    })
#define RELEASE_DEVICE() rg_mutex_give(audio.lock)

static struct
{
    const rg_audio_sink_t *sink;
    const rg_audio_driver_t *driver;
    rg_mutex_t *lock;
    int sampleRate;
    int filter;
    int volume;
    bool muted;
} audio;
static rg_audio_counters_t counters;

static const char *SETTING_DRIVER = "AudioDriver";
static const char *SETTING_DEVICE = "AudioDevice";
static const char *SETTING_VOLUME = "Volume";
static const char *SETTING_FILTER = "AudioFilter";

static const char *get_last_driver_error(void)
{
    if (audio.driver && audio.driver->get_error)
        return audio.driver->get_error();
    return "Unspecified Error";
}

static bool last_reset_was_brownout(void)
{
#ifdef ESP_PLATFORM
    return esp_reset_reason() == ESP_RST_BROWNOUT;
#else
    return false;
#endif
}

void rg_audio_init(int sampleRate)
{
    RG_ASSERT(audio.sink == NULL, "Audio sink already initialized!");

    if (!audio.lock)
    {
        audio.lock = rg_mutex_create();
        RELEASE_DEVICE();
    }
    ACQUIRE_DEVICE(1000);

    char *driver_name = rg_settings_get_string(NS_GLOBAL, SETTING_DRIVER, "DEFAULT");
    int device = rg_settings_get_number(NS_GLOBAL, SETTING_DEVICE, 0);
    for (size_t i = 0; i < RG_COUNT(sinks); ++i)
    {
        if (strcmp(sinks[i].driver->name, driver_name) == 0 && sinks[i].device == device)
            audio.sink = &sinks[i];
    }
    free(driver_name);

    if (!audio.sink) // Default to first non-dummy if no match found
        audio.sink = &sinks[1 % RG_COUNT(sinks)];

#ifdef RG_AUDIO_NO_HOTPLUG_SINK
    // This app has too little internal SRAM left for the USB host stack (see
    // components/retro-go/CMakeLists.txt). Honour the user's saved choice
    // everywhere else, but play through the built-in output here rather than
    // dropping out after a few seconds.
    if (audio.sink && audio.sink->driver->device_ready)
    {
        RG_LOGW("Sink '%s' needs more internal RAM than this app can spare, using '%s'\n",
                audio.sink->name, sinks[1 % RG_COUNT(sinks)].name);
        audio.sink = &sinks[1 % RG_COUNT(sinks)];
    }
#endif

    // Brown-out guard: hot-pluggable sinks (USB-C DAC) must power a 5V VBUS rail
    // that can collapse the supply on marginal hardware. If the previous boot
    // reset via brown-out while such a sink was selected, don't try it again --
    // revert to the speaker and persist it so we can't loop on every boot.
    if (audio.sink->driver->device_ready && last_reset_was_brownout())
    {
        RG_LOGW("Previous boot browned out with sink '%s'; reverting to speaker\n", audio.sink->name);
        audio.sink = &sinks[1 % RG_COUNT(sinks)];
        rg_settings_set_string(NS_GLOBAL, SETTING_DRIVER, audio.sink->driver->name);
        rg_settings_set_number(NS_GLOBAL, SETTING_DEVICE, audio.sink->device);
        rg_settings_commit();
    }

    audio.filter = (int)rg_settings_get_number(NS_GLOBAL, SETTING_FILTER, 0);
    audio.volume = (int)rg_settings_get_number(NS_GLOBAL, SETTING_VOLUME, 50);
    audio.sampleRate = sampleRate;
    audio.driver = audio.sink->driver;

    if (audio.driver->init(audio.sink->device, sampleRate))
    {
        if (audio.driver->set_mute)
            audio.driver->set_mute(audio.muted);
        if (audio.driver->set_volume)
            audio.driver->set_volume(audio.volume);

        RG_LOGI("Audio ready. sink='%s', samplerate=%d, volume=%d\n",
            audio.sink->name, audio.sampleRate, audio.volume);
    }
    else
    {
        RG_LOGE("Failed to initialize audio. sink='%s', samplerate=%d, volume=%d\n",
            audio.sink->name, audio.sampleRate, audio.volume);
        RG_LOGE(" - Error: %s\n", get_last_driver_error());
        audio.sink = &sinks[0]; // Switching to dummy might allow us to at least boot
        audio.driver = audio.sink->driver;
    }

    RELEASE_DEVICE();
}

void rg_audio_deinit(void)
{
    if (!audio.sink)
        return;

    // We'll go ahead even if we can't acquire the lock...
    ACQUIRE_DEVICE(1000);

    audio.driver->deinit();

    RG_LOGI("Audio terminated. sink='%s'\n", audio.sink->name);

    audio.driver = NULL;
    audio.sink = NULL;

    RELEASE_DEVICE();
}

void rg_audio_submit(const rg_audio_frame_t *frames, size_t count)
{
    const int64_t time_start = rg_system_timer();

    if (!audio.driver)
        return;

    if (!frames || !count)
        return;

    if (ACQUIRE_DEVICE(0))
    {
        audio.driver->submit(frames, count);
        RELEASE_DEVICE();
    }

    counters.totalSamples += count;
    counters.busyTime += rg_system_timer() - time_start;
}

rg_audio_counters_t rg_audio_get_counters(void)
{
    return counters;
}

const char *rg_audio_get_driver(void)
{
    if (!audio.driver)
        return NULL;
    return audio.driver->name;
}

const rg_audio_sink_t *rg_audio_get_sinks(size_t *count)
{
    if (count)
        *count = RG_COUNT(sinks);
    return sinks;
}

const rg_audio_sink_t *rg_audio_get_sink(void)
{
    return audio.sink;
}

bool rg_audio_sink_ready(void)
{
    // Sinks backed by a hot-pluggable device (e.g. USB-C DAC) only become
    // "ready" once that device has actually enumerated. Sinks without the hook
    // are ready as soon as the driver is initialized.
    if (audio.driver && audio.driver->device_ready)
        return audio.driver->device_ready();
    return audio.driver != NULL;
}

void rg_audio_save_sink(const char *driver_name, int device)
{
    // Persist the sink choice to flash WITHOUT bringing it up live. Used for
    // sinks that must be initialized at boot (USB-C DAC) so the caller can save
    // the choice and reboot rather than risk a live brown-out. rg_audio_init()
    // applies it on the next boot.
    RG_LOGI("save sink %s %d", driver_name, device);
    rg_settings_set_string(NS_GLOBAL, SETTING_DRIVER, driver_name);
    rg_settings_set_number(NS_GLOBAL, SETTING_DEVICE, device);
    rg_settings_commit();
}

void rg_audio_set_sink(const char *driver_name, int device)
{
    RG_LOGI("%s %d", driver_name, device);
    rg_settings_set_string(NS_GLOBAL, SETTING_DRIVER, driver_name);
    rg_settings_set_number(NS_GLOBAL, SETTING_DEVICE, device);
    rg_audio_deinit();
    rg_audio_init(audio.sampleRate);
}

int rg_audio_get_volume(void)
{
    return audio.volume;
}

void rg_audio_set_volume(int percent)
{
    audio.volume = RG_MIN(RG_MAX(percent, 0), 100);
    rg_settings_set_number(NS_GLOBAL, SETTING_VOLUME, audio.volume);
    // Audio may not be initialized yet (e.g. the input task can fire a volume
    // key before rg_audio_init() has run during boot). The value is stored and
    // gets applied by rg_audio_init(), so just skip the driver call here.
    if (!audio.driver)
        return;
    if (audio.driver->set_volume)
        audio.driver->set_volume(audio.volume);
    RG_LOGI("Volume set to %d%%\n", audio.volume);
}

bool rg_audio_get_mute(void)
{
    return audio.muted;
}

void rg_audio_set_mute(bool mute)
{
    audio.muted = mute;
    // Audio may not be initialized yet; the mute state is remembered and applied
    // by rg_audio_init() once the driver is ready.
    if (!audio.driver)
        return;

    if (!ACQUIRE_DEVICE(1000))
        return;

    if (audio.driver->set_mute)
        audio.driver->set_mute(mute);

    RELEASE_DEVICE();
}

int rg_audio_get_sample_rate(void)
{
    return audio.sampleRate;
}

void rg_audio_set_sample_rate(int sampleRate)
{
    // Audio may not be initialized yet; remember the rate so rg_audio_init()
    // can pick it up instead of asserting.
    if (!audio.driver)
    {
        audio.sampleRate = sampleRate;
        return;
    }

    if (audio.sampleRate == sampleRate)
        return;

    if (audio.driver->set_sample_rate)
    {
        if (ACQUIRE_DEVICE(1000))
        {
            audio.driver->set_sample_rate(sampleRate);
            audio.sampleRate = sampleRate;
            RELEASE_DEVICE();
        }
    }
    else
    {
        rg_audio_deinit();
        rg_audio_init(sampleRate);
    }
}
