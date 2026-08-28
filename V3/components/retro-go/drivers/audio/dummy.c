#include "rg_system.h"
#include "rg_audio.h"

static int64_t busyUntil = 0;

static bool driver_init(int device, int sampleRate)
{
    busyUntil = 0;
    return true;
}

static bool driver_deinit(void)
{
    return true;
}

static bool driver_submit(const rg_audio_frame_t *frames, size_t count)
{
    int64_t now = rg_system_timer();
    if (busyUntil > now) {
        int64_t diff = busyUntil - now;
        if (diff > 100000) diff = 100000;
        rg_usleep((uint32_t)diff);
    }
    int sampleRate = rg_audio_get_sample_rate();
    if (sampleRate <= 0) sampleRate = 32000;
    busyUntil = rg_system_timer() + (int64_t)((float)count * (1000000.f / sampleRate));
    return true;
}

const rg_audio_driver_t rg_audio_driver_dummy = {
    .name = "dummy",
    .init = driver_init,
    .deinit = driver_deinit,
    .submit = driver_submit,
};
