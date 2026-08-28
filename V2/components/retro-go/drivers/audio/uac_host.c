/*

Audio driver for USB Audio Class (UAC) host
============================================
Routes retro-go's PCM audio over USB-C to an external USB Audio Class device
(typical USB-C -> 3.5mm DAC dongle). The ESP32-S3 acts as USB host; the dongle
appears as an isochronous OUT endpoint we feed stereo s16 frames into.

Lifecycle:
  driver_init   -> usb_host_install + spawn lib-event task + uac_host_install
                   + spawn worker task that handles device-connect requests
  TX_CONNECTED  -> lib callback enqueues {addr, iface_num} to the worker queue
                   and returns immediately (the IDF UAC component documents
                   that callbacks should be brief; doing device_open and
                   device_start from inside the callback observed to panic
                   during enumeration of typical USB-C DAC dongles).
  worker task   -> uac_host_device_open + uac_host_device_start, falling back
                   to 48 kHz when the requested rate isn't supported. Stores
                   handle into state for submit() to use.
  driver_submit -> linear-interpolating resampler + uac_host_device_write.
  DISCONNECTED  -> close handle, clear state. Driver stays alive for re-plug.
  driver_deinit -> close any open device, uninstall UAC + worker. The USB
                   host stack and its lib task are intentionally NOT torn
                   down here -- see comment in driver_init.

*/

#include "rg_system.h"
#include "rg_audio.h"

#if RG_AUDIO_USE_UAC_HOST

#ifndef ESP_PLATFORM
#error "USB Audio Class host driver requires esp-idf!"
#endif

#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <usb/usb_host.h>
#include <usb/uac_host.h>
#include <esp_err.h>

#define UAC_FALLBACK_SAMPLE_RATE   48000
#define UAC_RING_BUFFER_BYTES      4096
#define UAC_RING_THRESHOLD_BYTES   2048
#define UAC_WRITE_TIMEOUT_MS       50
#define UAC_USB_LIB_STACK          8192
#define UAC_BG_STACK               8192
#define UAC_WORKER_STACK           8192
#define UAC_WORKER_QUEUE_DEPTH     4

typedef struct {
    uint8_t addr;
    uint8_t iface_num;
} uac_connect_msg_t;

static struct {
    const char *last_error;
    int sample_rate;        // Source rate (whatever the emulator submits at)
    int uac_stream_rate;    // Rate the UAC stream is actually running at, 0 if no device
    uint8_t uac_channels;   // Wire format of the running stream (1 or 2 channels)
    uint8_t uac_bits;       // Wire format of the running stream (16, 24 or 32 bit)
    float resample_phase;   // Carryover phase between submit() calls (in source-frame units)
    int16_t last_left;      // Last source frame from previous submit, used to linear-interpolate
    int16_t last_right;     // across chunk boundaries (otherwise we'd glitch every chunk).
    int volume;
    bool muted;

    bool host_installed;
    TaskHandle_t usb_lib_task;
    TaskHandle_t worker_task;
    QueueHandle_t worker_queue;

    SemaphoreHandle_t dev_mutex;
    uac_host_device_handle_t dev_handle;

    int64_t silent_busy_until; // microsecond timestamp; pace submit() when no device is connected
} state;

// USB host library event loop. Required for the host stack to do anything;
// without this task running, devices never enumerate.
static void usb_lib_task(void *arg)
{
    (void)arg;
    while (1)
    {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}

// Per-device events. Fires from the UAC background task. We only act on
// disconnect; transfer errors get logged but submit() will surface them too.
static void uac_device_event_cb(uac_host_device_handle_t handle,
                                const uac_host_device_event_t event,
                                void *arg)
{
    (void)arg;
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED)
    {
        RG_LOGI("UAC: device disconnected\n");
        xSemaphoreTake(state.dev_mutex, portMAX_DELAY);
        if (state.dev_handle == handle)
        {
            state.dev_handle = NULL;
            state.uac_stream_rate = 0;
        }
        uac_host_device_close(handle);
        xSemaphoreGive(state.dev_mutex);
    }
    else if (event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR)
    {
        RG_LOGW("UAC: transfer error\n");
    }
}

// Pick the usable sample rate closest to `wanted` from one alt setting.
// Returns 0 when the alt has no usable rate.
static uint32_t uac_pick_rate(const uac_host_dev_alt_param_t *p, uint32_t wanted)
{
    if (p->sample_freq_type == 0) // Continuous range: clamp
    {
        if (wanted < p->sample_freq_lower) return p->sample_freq_lower;
        if (wanted > p->sample_freq_upper) return p->sample_freq_upper;
        return wanted;
    }
    int count = p->sample_freq_type;
    if (count > UAC_FREQ_NUM_MAX)
        count = UAC_FREQ_NUM_MAX;
    uint32_t best = 0;
    for (int i = 0; i < count; ++i)
    {
        uint32_t f = p->sample_freq[i];
        if (f == 0)
            continue;
        if (f == wanted)
            return f;
        // Prefer whichever is closest to the wanted rate; 48k wins ties by
        // being checked against the wanted-distance including a small bonus.
        uint32_t d_f = (f > wanted) ? f - wanted : wanted - f;
        uint32_t d_b = (best > wanted) ? best - wanted : wanted - best;
        if (best == 0 || d_f < d_b || (d_f == d_b && f == UAC_FALLBACK_SAMPLE_RATE))
            best = f;
    }
    return best;
}

// Worker task: actually opens the device and starts the stream after a
// connect event lands in the queue. Runs outside the UAC component's
// background task context, which keeps that callback path lightweight.
static void uac_worker_task(void *arg)
{
    (void)arg;
    uac_connect_msg_t msg;

    while (xQueueReceive(state.worker_queue, &msg, portMAX_DELAY) == pdTRUE)
    {
        RG_LOGI("UAC worker: opening device addr=%d iface=%d\n", msg.addr, msg.iface_num);

        const uac_host_device_config_t dev_cfg = {
            .addr = msg.addr,
            .iface_num = msg.iface_num,
            .buffer_size = UAC_RING_BUFFER_BYTES,
            .buffer_threshold = UAC_RING_THRESHOLD_BYTES,
            .callback = uac_device_event_cb,
            .callback_arg = NULL,
        };
        uac_host_device_handle_t handle = NULL;
        esp_err_t err = uac_host_device_open(&dev_cfg, &handle);
        if (err != ESP_OK)
        {
            RG_LOGE("UAC worker: device_open failed: %s\n", esp_err_to_name(err));
            continue;
        }

        // Negotiate the wire format. The old behaviour (16-bit stereo at the
        // source rate, else 48 kHz) rejected dongles that only do 24-bit,
        // 44.1 kHz or mono, so walk every alt setting and score them: we can
        // convert to any of 16/24/32-bit and 1/2 channels, and the resampler
        // handles any rate, so almost any PCM alt is usable -- we just prefer
        // the cheapest one.
        uac_host_stream_config_t best_cfg = {0};
        int best_score = 0;
        uac_host_dev_info_t info;
        if (uac_host_get_device_info(handle, &info) == ESP_OK)
        {
            RG_LOGI("UAC worker: VID=%04X PID=%04X, %d alt setting(s)\n",
                    info.VID, info.PID, info.iface_alt_num);
            for (int alt = 1; alt <= info.iface_alt_num; ++alt)
            {
                uac_host_dev_alt_param_t p;
                if (uac_host_get_device_alt_param(handle, alt, &p) != ESP_OK)
                    continue;
                uint32_t rate = uac_pick_rate(&p, state.sample_rate);
                RG_LOGI("UAC worker: alt %d: format=%d ch=%d bits=%d rate=%lu\n",
                        alt, p.format, p.channels, p.bit_resolution, (unsigned long)rate);
                if (p.format != 1 /* PCM */ || rate == 0)
                    continue;
                if (p.channels != 1 && p.channels != 2)
                    continue;
                if (p.bit_resolution != 16 && p.bit_resolution != 24 && p.bit_resolution != 32)
                    continue;
                int score = 1; // Any usable alt beats "nothing found"
                score += (p.bit_resolution == 16) ? 400 : (p.bit_resolution == 24) ? 200 : 100;
                score += (p.channels == 2) ? 200 : 50;
                if (rate == (uint32_t)state.sample_rate)
                    score += 300;
                else if (rate == UAC_FALLBACK_SAMPLE_RATE)
                    score += 150;
                if (score > best_score)
                {
                    best_score = score;
                    best_cfg = (uac_host_stream_config_t){
                        .channels = p.channels,
                        .bit_resolution = p.bit_resolution,
                        .sample_freq = rate,
                    };
                }
            }
        }

        // Attempts: negotiated format first, then the legacy guesses in case
        // enumeration failed or the device lied about an alt setting.
        const uac_host_stream_config_t attempts[] = {
            best_cfg,
            {.channels = 2, .bit_resolution = 16, .sample_freq = state.sample_rate},
            {.channels = 2, .bit_resolution = 16, .sample_freq = UAC_FALLBACK_SAMPLE_RATE},
        };
        uac_host_stream_config_t stream_cfg = {0};
        err = ESP_ERR_NOT_FOUND;
        for (size_t i = (best_score > 0) ? 0 : 1; i < RG_COUNT(attempts) && err != ESP_OK; ++i)
        {
            stream_cfg = attempts[i];
            err = uac_host_device_start(handle, &stream_cfg);
            if (err != ESP_OK)
                RG_LOGW("UAC worker: start %dch/%d-bit @ %lu Hz failed: %s\n",
                        stream_cfg.channels, stream_cfg.bit_resolution,
                        (unsigned long)stream_cfg.sample_freq, esp_err_to_name(err));
        }
        if (err != ESP_OK)
        {
            RG_LOGE("UAC worker: no usable format, dumping device params:\n");
            uac_host_printf_device_param(handle); // Full descriptor dump for diagnosis
            uac_host_device_close(handle);
            continue;
        }

        // Publish the handle and the wire format the device is running at.
        // submit() reads these to drive the resampler when source != stream
        // rate (typical: 32 kHz emulator audio over a 48 kHz UAC dongle) and
        // to encode frames when the wire format isn't 16-bit stereo.
        xSemaphoreTake(state.dev_mutex, portMAX_DELAY);
        state.dev_handle = handle;
        state.uac_stream_rate = stream_cfg.sample_freq;
        state.uac_channels = stream_cfg.channels;
        state.uac_bits = stream_cfg.bit_resolution;
        state.resample_phase = 0.0f;
        state.last_left = 0;
        state.last_right = 0;
        xSemaphoreGive(state.dev_mutex);

        RG_LOGI("UAC worker: stream started @ %lu Hz, %d-bit, %dch\n",
                (unsigned long)stream_cfg.sample_freq, stream_cfg.bit_resolution,
                stream_cfg.channels);
    }
}

// UAC driver-level events: keep this minimal -- just enqueue.
static void uac_driver_event_cb(uint8_t addr, uint8_t iface_num,
                                const uac_host_driver_event_t event, void *arg)
{
    (void)arg;
    if (event != UAC_HOST_DRIVER_EVENT_TX_CONNECTED)
        return; // RX/microphone events ignored

    if (!state.worker_queue)
        return; // racing with deinit

    const uac_connect_msg_t msg = {.addr = addr, .iface_num = iface_num};
    if (xQueueSend(state.worker_queue, &msg, 0) != pdTRUE)
        RG_LOGW("UAC: connect queue full, dropping event addr=%d\n", addr);
}

static bool driver_init(int device, int sample_rate)
{
    (void)device;
    state.last_error = NULL;
    state.sample_rate = sample_rate;
    state.dev_handle = NULL;

    if (!state.dev_mutex)
        state.dev_mutex = xSemaphoreCreateMutex();
    if (!state.dev_mutex)
    {
        state.last_error = "Failed to create UAC mutex";
        return false;
    }

    // The USB host stack and its event-loop task are installed once for the
    // firmware's lifetime. Properly tearing them down requires waiting for
    // an ALL_FREE event after every client deregisters and every device
    // closes -- the lib task is responsible for delivering that event, so we
    // can't just delete it. Leaving the host running across audio-sink
    // switches is correct behaviour and avoids the INVALID_STATE we hit on
    // re-installing a partly-released host.
    if (!state.host_installed)
    {
        const usb_host_config_t host_cfg = {
            .skip_phy_setup = false,
            .intr_flags = ESP_INTR_FLAG_LEVEL1,
        };
        esp_err_t err = usb_host_install(&host_cfg);
        if (err != ESP_OK)
        {
            state.last_error = esp_err_to_name(err);
            RG_LOGE("UAC: usb_host_install failed: %s\n", state.last_error);
            return false;
        }
        state.host_installed = true;

        BaseType_t ok = xTaskCreatePinnedToCore(
            usb_lib_task, "usb_lib", UAC_USB_LIB_STACK, NULL, 5, &state.usb_lib_task, 0);
        if (ok != pdPASS)
        {
            state.last_error = "Failed to spawn usb_lib task";
            return false;
        }
    }

    state.worker_queue = xQueueCreate(UAC_WORKER_QUEUE_DEPTH, sizeof(uac_connect_msg_t));
    if (!state.worker_queue)
    {
        state.last_error = "Failed to create UAC worker queue";
        return false;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        uac_worker_task, "uac_worker", UAC_WORKER_STACK, NULL, 4, &state.worker_task, 0);
    if (ok != pdPASS)
    {
        state.last_error = "Failed to spawn uac_worker task";
        vQueueDelete(state.worker_queue);
        state.worker_queue = NULL;
        return false;
    }

    const uac_host_driver_config_t uac_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = UAC_BG_STACK,
        .core_id = 0,
        .callback = uac_driver_event_cb,
        .callback_arg = NULL,
    };
    esp_err_t err = uac_host_install(&uac_cfg);
    if (err != ESP_OK)
    {
        state.last_error = esp_err_to_name(err);
        RG_LOGE("UAC: uac_host_install failed: %s\n", state.last_error);
        vTaskDelete(state.worker_task);
        state.worker_task = NULL;
        vQueueDelete(state.worker_queue);
        state.worker_queue = NULL;
        return false;
    }

    RG_LOGI("UAC: host ready, waiting for device (target sample_rate=%d)\n", sample_rate);
    return true;
}

static bool driver_deinit(void)
{
    xSemaphoreTake(state.dev_mutex, portMAX_DELAY);
    if (state.dev_handle)
    {
        uac_host_device_close(state.dev_handle);
        state.dev_handle = NULL;
    }
    xSemaphoreGive(state.dev_mutex);

    // Tear down only the UAC client layer + worker. Leave the USB host stack
    // and its lib task running -- see comment in driver_init for why.
    uac_host_uninstall();

    if (state.worker_task)
    {
        vTaskDelete(state.worker_task);
        state.worker_task = NULL;
    }
    if (state.worker_queue)
    {
        vQueueDelete(state.worker_queue);
        state.worker_queue = NULL;
    }
    return true;
}

static bool uac_write_bytes(uac_host_device_handle_t handle, uint8_t *data, size_t size)
{
    esp_err_t err = uac_host_device_write(handle, data, size, pdMS_TO_TICKS(UAC_WRITE_TIMEOUT_MS));
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
    {
        RG_LOGW("UAC: write failed: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool uac_flush_scratch(uac_host_device_handle_t handle,
                              const rg_audio_frame_t *scratch, size_t out_count)
{
    if (out_count == 0)
        return true;

    // Native wire format (16-bit stereo): write the frames as-is.
    if (state.uac_channels == 2 && state.uac_bits == 16)
        return uac_write_bytes(handle, (uint8_t *)scratch, out_count * sizeof(rg_audio_frame_t));

    // Encode to the negotiated wire format (mono downmix and/or 24/32-bit
    // little-endian, zero-padded low bytes). Static because 2KB is too big
    // for the emulator task's stack; submit() has a single caller so this
    // isn't reentered.
    static uint8_t wire[256 * 2 * 4]; // Matches scratch[256] at worst case 2ch * 32-bit
    const int bytes = state.uac_bits / 8;
    size_t w = 0;
    for (size_t i = 0; i < out_count; ++i)
    {
        int16_t samples[2] = {scratch[i].left, scratch[i].right};
        int nch = state.uac_channels;
        if (nch == 1)
            samples[0] = (int16_t)(((int32_t)scratch[i].left + scratch[i].right) / 2);
        for (int c = 0; c < nch; ++c)
        {
            if (bytes == 4) { wire[w++] = 0; wire[w++] = 0; }
            else if (bytes == 3) { wire[w++] = 0; }
            wire[w++] = (uint8_t)(samples[c] & 0xFF);
            wire[w++] = (uint8_t)((samples[c] >> 8) & 0xFF);
        }
    }
    return uac_write_bytes(handle, wire, w);
}

static bool driver_submit(const rg_audio_frame_t *frames, size_t count)
{
    if (state.muted || count == 0)
        return true;

    xSemaphoreTake(state.dev_mutex, portMAX_DELAY);
    uac_host_device_handle_t handle = state.dev_handle;
    int out_rate = state.uac_stream_rate;
    float phase = state.resample_phase;
    xSemaphoreGive(state.dev_mutex);

    const int in_rate = state.sample_rate;  // declared here so the no-device path below can use it
    const float vol = state.volume * 0.01f;

    if (!handle)
    {
        // No device connected. We can't return immediately -- the emulator's
        // frame-pacing depends on submit() taking real time (otherwise it
        // races at full CPU). Sleep for the playback duration of the chunk,
        // same trick the dummy driver uses.
        int64_t now = rg_system_timer();
        if (state.silent_busy_until > now)
            rg_usleep(state.silent_busy_until - now);
        state.silent_busy_until = rg_system_timer() +
            (int64_t)((float)count * (1000000.0f / (float)in_rate));
        return true;
    }
    rg_audio_frame_t scratch[256];

    if (out_rate == 0 || out_rate == in_rate)
    {
        // No resampling needed -- fast path with volume only.
        size_t pos = 0;
        while (pos < count)
        {
            size_t chunk = count - pos;
            if (chunk > RG_COUNT(scratch))
                chunk = RG_COUNT(scratch);
            for (size_t i = 0; i < chunk; ++i)
            {
                scratch[i].left  = (int16_t)(frames[pos + i].left  * vol);
                scratch[i].right = (int16_t)(frames[pos + i].right * vol);
            }
            if (!uac_flush_scratch(handle, scratch, chunk))
                break;
            pos += chunk;
        }
        return true;
    }

    // Linear-interpolating resampler from in_rate -> out_rate. phase carries
    // a fractional position across submit() calls so the rate stays exact
    // (no integer-step drift); state.last_{left,right} carries the previous
    // chunk's final frame so the very first interpolation in this chunk can
    // span the chunk boundary without a discontinuity. The phase carryover
    // is in the range (-1, 0], so the first iteration may see idx == -1,
    // which we resolve to state.last_*.
    const float step = (float)in_rate / (float)out_rate;
    int16_t prev_left = state.last_left;
    int16_t prev_right = state.last_right;
    size_t out_pos = 0;
    while (1)
    {
        int idx = (int)floorf(phase);
        if (idx + 1 >= (int)count)
            break; // Need both frames[idx] and frames[idx+1].

        float frac = phase - (float)idx;

        int16_t a_left  = (idx < 0) ? prev_left  : frames[idx].left;
        int16_t a_right = (idx < 0) ? prev_right : frames[idx].right;
        int16_t b_left  = frames[idx + 1].left;
        int16_t b_right = frames[idx + 1].right;

        float lerp_l = a_left  + ((float)b_left  - (float)a_left ) * frac;
        float lerp_r = a_right + ((float)b_right - (float)a_right) * frac;

        scratch[out_pos].left  = (int16_t)(lerp_l * vol);
        scratch[out_pos].right = (int16_t)(lerp_r * vol);
        out_pos++;
        phase += step;

        if (out_pos >= RG_COUNT(scratch))
        {
            if (!uac_flush_scratch(handle, scratch, out_pos))
            {
                out_pos = 0;
                break;
            }
            out_pos = 0;
        }
    }
    uac_flush_scratch(handle, scratch, out_pos);

    // Carry the unresolved phase + the trailing source frame into the next
    // submit. The next chunk's idx == -1 will then resolve to this value.
    phase -= (float)count;
    if (phase < -1.0f) // Defensive: tiny chunks with big steps could undershoot.
        phase = 0.0f;
    xSemaphoreTake(state.dev_mutex, portMAX_DELAY);
    state.resample_phase = phase;
    state.last_left  = frames[count - 1].left;
    state.last_right = frames[count - 1].right;
    xSemaphoreGive(state.dev_mutex);

    return true;
}

static bool driver_set_sample_rate(int sample_rate)
{
    state.sample_rate = sample_rate;
    return true;
}

static bool driver_set_volume(int volume)
{
    state.volume = volume;
    return true;
}

static bool driver_set_mute(bool mute)
{
    state.muted = mute;
    return true;
}

static const char *driver_get_error(void)
{
    return state.last_error;
}

const rg_audio_driver_t rg_audio_driver_uac_host = {
    .name = "uac_host",
    .init = driver_init,
    .deinit = driver_deinit,
    .submit = driver_submit,
    .set_mute = driver_set_mute,
    .set_volume = driver_set_volume,
    .set_sample_rate = driver_set_sample_rate,
    .get_error = driver_get_error,
};

#endif // RG_AUDIO_USE_UAC_HOST
