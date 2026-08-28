#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>

#include "../components/gbsp-libretro/common.h"
#include "../components/gbsp-libretro/memmap.h"
#include "../components/gbsp-libretro/sound.h"
#include "../components/gbsp-libretro/gba_memory.h"
#include "../components/gbsp-libretro/gba_cc_lut.h"
#include "../components/gbsp-libretro/cpu_jit.h"

#ifdef ESP_PLATFORM
#include "esp_mmu_map.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
// GBA Link over ESP-NOW: implemented but DISABLED. The WiFi stack's static .bss
// (libnet80211/libpp/libphy, ~40KB internal DRAM) does not fit alongside the GBA
// core's internal buffers (96KB vram + 32KB iwram + JIT). Enabling pulls the WiFi
// libs in and overflows dram0 by ~40KB. See handleidingen2/GBA_LINK_HANDLEIDING.md.
#define GBA_LINK_NETLINK 0
#if GBA_LINK_NETLINK
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include <string.h>
#endif
#endif

#define AUDIO_SAMPLE_RATE (GBA_SOUND_FREQUENCY)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 60 + 1)

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;
boot_mode selected_boot_mode = boot_game;

u32 skip_next_frame = 0;
int sprite_limit = 1;

// jit_test.c — haalbaarheidstest executable PSRAM (zie P4 JIT-traject)
extern bool jit_test_run(void);
// jit_selftest.c — stage 1 Xtensa JIT: emitter/ABI/literal/callx8 pipeline test
extern bool jit_selftest_run(void);

gbsp_memory_t *gbsp_memory;
u8 (*gbsp_vram)[1024 * 96];
u8 (*gbsp_iwram)[(1024 * 32) << SMC_DETECTION];

static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_app_t *app;

static const char *SETTING_SOUND_EMULATION = "sound";
static const char *SETTING_FRAMESKIP = "frameskip";
static const char *SETTING_JIT = "jit"; // 0 = classic interpreter, 1 = JIT
#define JIT_DEFAULT 1  // on out of the box: this is what makes demanding
                       // games run at full speed. Options > CPU core turns
                       // it off again if a game ever misbehaves.
static int user_frameskip = 0; // 0 = auto (skip only when the emulation is behind schedule)

/* Work around the JIT cold-boot hang: the JIT gets stuck in the boot poll loop
 * (never sees the VBlank IRQ flag → white screen); the interpreter doesn't. So run
 * the one-time (re)boot in classic and only enable JIT once the game renders
 * (DISPCNT forced-blank off). Applies to new game and menu reset; resume skips it. */
static int jit_boot_defer = 0, jit_boot_frames = 0;
static void jit_defer_boot(void)
{
    if (rg_settings_get_number(NS_APP, SETTING_JIT, JIT_DEFAULT))  // source of truth = the user setting
    {
        jit_enabled = 0;            // boot in the interpreter
        jit_boot_defer = 1;
        jit_boot_frames = 0;
    }
}

#ifdef ESP_PLATFORM
// Cache sync for the JIT: D-cache writeback (write view) + I-cache
// invalidate (exec view, 64B-aligned).
void jit_cache_sync(void *wr_addr, void *ex_addr, u32 len)
{
    esp_cache_msync(wr_addr, len,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    uintptr_t ex = (uintptr_t)ex_addr & ~63u;
    u32 ex_len = (((uintptr_t)ex_addr + len + 63) & ~63u) - ex;
    esp_cache_msync((void *)ex, ex_len,
        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
}
#else
void jit_cache_sync(void *wr_addr, void *ex_addr, u32 len) {}
#endif

// ===========================================================================
// GBA Link transport — ESP-NOW backend for the Wireless Adapter (RFU) emulation.
// rfu.c talks to the outside world via netpacket_send / netpacket_poll_receive
// (outgoing) and rfu_net_receive (incoming). On RetroArch these map to the
// netpacket interface; here we map them to ESP-NOW (peer-to-peer, no TCP/IP).
//
// Stage 0 (PoC): 2 players, best-effort. Each device learns the peer's MAC from
// the first received packet and assigns stable client-ids by MAC comparison
// (lower MAC = id 0). Reliability shim (seq/ack/retransmit/dedup) = stage 1.
// ===========================================================================
#if defined(ESP_PLATFORM) && GBA_LINK_NETLINK
#define NETLINK_CHANNEL  6
static const uint8_t NETLINK_BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static volatile bool netlink_up    = false;
static volatile bool netlink_peer  = false;   // peer MAC learned
static uint8_t  netlink_peer_mac[6];
static uint16_t netlink_my_id  = 0;
static uint16_t netlink_peer_id = 1;

// Single-producer (WiFi recv cb) / single-consumer (emu task) ring buffer.
#define NETLINK_RX_RING 32
#define NETLINK_PKT_MAX 250
typedef struct { uint8_t data[NETLINK_PKT_MAX]; uint16_t len; uint16_t from; } netlink_rx_t;
static netlink_rx_t   netlink_rx[NETLINK_RX_RING];
static volatile int   netlink_rx_head = 0, netlink_rx_tail = 0;

static void netlink_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len <= 0 || len > NETLINK_PKT_MAX)
        return;

    if (!netlink_peer)
    {
        memcpy(netlink_peer_mac, info->src_addr, 6);
        uint8_t my[6] = {0};
        esp_wifi_get_mac(WIFI_IF_STA, my);
        // Deterministic, symmetric id assignment: lower MAC = id 0.
        netlink_my_id  = (memcmp(my, netlink_peer_mac, 6) < 0) ? 0 : 1;
        netlink_peer_id = netlink_my_id ^ 1;
        esp_now_peer_info_t p = {0};
        memcpy(p.peer_addr, netlink_peer_mac, 6);
        p.channel = NETLINK_CHANNEL;
        p.ifidx   = WIFI_IF_STA;
        esp_now_add_peer(&p);
        netlink_peer = true;
    }

    int next = (netlink_rx_head + 1) % NETLINK_RX_RING;
    if (next == netlink_rx_tail)
        return;   // ring full → drop (best-effort; shim handles loss in stage 1)
    memcpy(netlink_rx[netlink_rx_head].data, data, len);
    netlink_rx[netlink_rx_head].len  = (uint16_t)len;
    netlink_rx[netlink_rx_head].from = netlink_peer_id;
    netlink_rx_head = next;
}

static void netlink_start(void)
{
    if (netlink_up)
        return;

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_event_loop_create_default();        // returns INVALID_STATE if already done; ok
    // (no esp_netif_init — ESP-NOW is MAC-layer only, no IP stack needed)

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) { RG_LOGE("GBA Link: wifi_init failed (low RAM?)"); return; }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) { RG_LOGE("GBA Link: wifi_start failed"); return; }
    esp_wifi_set_ps(WIFI_PS_NONE);          // no power-save → no TX latency (link timing)
    esp_wifi_set_channel(NETLINK_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) { RG_LOGE("GBA Link: esp_now_init failed"); return; }
    esp_now_register_recv_cb(netlink_recv_cb);

    esp_now_peer_info_t bc = {0};           // broadcast peer for discovery
    memcpy(bc.peer_addr, NETLINK_BCAST, 6);
    bc.channel = NETLINK_CHANNEL;
    bc.ifidx   = WIFI_IF_STA;
    esp_now_add_peer(&bc);

    netlink_up = true;
    RG_LOGI("GBA Link: ESP-NOW up on channel %d", NETLINK_CHANNEL);
}

void netpacket_poll_receive()
{
    while (netlink_rx_tail != netlink_rx_head)
    {
        netlink_rx_t *p = &netlink_rx[netlink_rx_tail];
        rfu_net_receive(p->data, p->len, p->from);
        netlink_rx_tail = (netlink_rx_tail + 1) % NETLINK_RX_RING;
    }
}

void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
    if (!netlink_up || len == 0 || len > NETLINK_PKT_MAX)
        return;
    // 2-player: any destination (specific id or 0xffff broadcast) is the one peer.
    const uint8_t *dst = netlink_peer ? netlink_peer_mac : NETLINK_BCAST;
    esp_now_send(dst, (const uint8_t *)buf, len);
}
#else
void netpacket_poll_receive() {}
void netpacket_send(uint16_t client_id, const void *buf, size_t len) {}
#endif

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

// Stats overlay label (rg_display weak hook): shows the live CPU core in the border.
const char *rg_display_osd_tag(void)
{
    return jit_enabled ? "jit" : "cla";
}

static bool save_state_handler(const char *filename)
{
    size_t buffer_len = GBA_STATE_MEM_SIZE;
    gamepak_cache_release_partial();   // ROM cache yields temporarily for the big buffer
    void *buffer = rg_alloc(buffer_len, MEM_ANY);
    if (!buffer)
    {
        RG_LOGE("Failed to allocate %d bytes for save state!", (int)buffer_len);
        gamepak_cache_restore_partial();
        return false;
    }
    gba_save_state(buffer);
    bool success = rg_storage_write_file(filename, buffer, buffer_len, 0);
    free(buffer);
    gamepak_cache_restore_partial();
    return success;
}

static bool load_state_handler(const char *filename)
{
    size_t buffer_len = GBA_STATE_MEM_SIZE;
    gamepak_cache_release_partial();   // ROM cache yields temporarily for the big buffer
    void *buffer = rg_alloc(buffer_len, MEM_ANY);
    if (!buffer)
    {
        RG_LOGE("Failed to allocate %d bytes for load state!", (int)buffer_len);
        gamepak_cache_restore_partial();
        return false;
    }
    bool success = rg_storage_read_file(filename, &buffer, &buffer_len, RG_FILE_USER_BUFFER)
                    && gba_load_state(buffer);
    free(buffer);
    gamepak_cache_restore_partial();
    jit_invalidate_all(); // JIT cache is derived state
    return success;
}

static bool reset_handler(bool hard)
{
    reset_gba();
    jit_invalidate_all();
    jit_defer_boot();   // run the boot in classic after a menu reset too
    return true;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(currentUpdate, 0);
    }
}

int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    // RG_LOGI("%u, %u, %u, %u", port, device, index, id);
    uint32_t joystick = rg_input_read_gamepad();
    int16_t val = 0;
    if (joystick & RG_KEY_DOWN) val |= (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (joystick & RG_KEY_UP) val |= (1 << RETRO_DEVICE_ID_JOYPAD_UP);
    if (joystick & RG_KEY_LEFT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (joystick & RG_KEY_RIGHT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (joystick & RG_KEY_START) val |= (1 << RETRO_DEVICE_ID_JOYPAD_START);
    if (joystick & RG_KEY_SELECT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_SELECT);
    if (joystick & RG_KEY_B) val |= (1 << RETRO_DEVICE_ID_JOYPAD_B);
    if (joystick & RG_KEY_A) val |= (1 << RETRO_DEVICE_ID_JOYPAD_A);
    if (joystick & (RG_KEY_X | RG_KEY_Y)) val |= (1 << RETRO_DEVICE_ID_JOYPAD_L);  // Brightness UP/DOWN = GBA L
    if (joystick & (RG_KEY_L | RG_KEY_R)) val |= (1 << RETRO_DEVICE_ID_JOYPAD_R);  // Volume UP/DOWN = GBA R
    return val;
}

void set_fastforward_override(bool fastforward)
{
}

static rg_gui_event_t sound_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        sound_master_enable = !sound_master_enable;
        rg_settings_set_number(NS_APP, SETTING_SOUND_EMULATION, sound_master_enable);
    }

    strcpy(option->value, sound_master_enable ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t frameskip_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV && user_frameskip > -1)   // -1 = Off (never skip)
    {
        user_frameskip--;
        app->frameskip = user_frameskip;
        rg_settings_set_number(NS_APP, SETTING_FRAMESKIP, user_frameskip);
    }
    else if (event == RG_DIALOG_NEXT && user_frameskip < 9)
    {
        user_frameskip++;
        app->frameskip = user_frameskip;
        rg_settings_set_number(NS_APP, SETTING_FRAMESKIP, user_frameskip);
    }

    if (user_frameskip < 0)
        strcpy(option->value, _("Off"));
    else if (user_frameskip == 0)
        strcpy(option->value, _("Auto"));
    else
        sprintf(option->value, "%d", user_frameskip);

    return RG_DIALOG_VOID;
}

static rg_gui_event_t volume_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int volume = rg_audio_get_volume();
    if (event == RG_DIALOG_PREV && volume > 0)
        rg_audio_set_volume(volume - 5);
    else if (event == RG_DIALOG_NEXT && volume < 100)
        rg_audio_set_volume(volume + 5);
    sprintf(option->value, "%d%%", rg_audio_get_volume());
    return RG_DIALOG_VOID;
}

static rg_gui_event_t brightness_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int brightness = rg_display_get_backlight();
    if (event == RG_DIALOG_PREV && brightness > 1)
        rg_display_set_backlight(brightness - 10);
    else if (event == RG_DIALOG_NEXT && brightness < 100)
        rg_display_set_backlight(brightness + 10);
    sprintf(option->value, "%d%%", rg_display_get_backlight());
    return RG_DIALOG_VOID;
}

static rg_gui_event_t jit_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        jit_enabled = !jit_enabled;
        rg_settings_set_number(NS_APP, SETTING_JIT, jit_enabled);
        jit_invalidate_all();
    }
    strcpy(option->value, jit_enabled ? _("JIT") : _("Classic"));
    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Frameskip"), "-", RG_DIALOG_FLAG_NORMAL, &frameskip_cb};
    *dest++ = (rg_gui_option_t){0, _("Audio enable"), "-", RG_DIALOG_FLAG_NORMAL, &sound_toggle_cb};
    *dest++ = (rg_gui_option_t){0, _("CPU core"), "-", RG_DIALOG_FLAG_NORMAL, &jit_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

#if RENDER_OFFLOAD
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_cpu.h"

/* The core0 render task + producer-consumer sync.
 *  - emu core (main loop): clean frame -> render_snap_capture() to g_snap, gives snap_ready.
 *  - render task (other core): waits snap_ready -> renders g_snap -> target -> blit -> gives snap_free.
 *  - the emu core waits on snap_free before the next capture (render done with g_snap). In
 *    steady state never blocking (render ~7ms < frame ~16ms). The render CPU overlaps the
 *    blit SPI wait -> pipeline -> towards 60fps. */
extern u32 g_m2_dirty;
static SemaphoreHandle_t s_snap_ready, s_snap_free;
static rg_surface_t *s_render_target;
static volatile bool s_offload_run, s_offload_was_active;

static void render_offload_task(void *arg)
{
    while (s_offload_run)
    {
        if (xSemaphoreTake(s_snap_ready, pdMS_TO_TICKS(200)) != pdTRUE)
            continue;
        render_offload_frame((u16 *)s_render_target->data);  // g_snap -> target (this core)
        rg_display_submit(s_render_target, 0);               // blit
        xSemaphoreGive(s_snap_free);
    }
    vTaskDelete(NULL);
}

static void render_offload_start(void)
{
    s_snap_ready = xSemaphoreCreateBinary();
    s_snap_free  = xSemaphoreCreateBinary();
    xSemaphoreGive(s_snap_free);   // g_snap is initially free
    s_offload_run = true;
    s_offload_was_active = false;
    int emu_core = esp_cpu_get_core_id();
    // Pin to the non-emu core (so emu and render are on different cores -> the
    // per-core R_CTX works, and the render overlaps the blit). Prio < display (6) so
    // the display task can preempt it to blit.
    xTaskCreatePinnedToCore(render_offload_task, "rofl", 4096, NULL, 5, NULL, 1 - emu_core);
}
#endif

void app_main(void)
{
    const rg_handlers_t handlers = {
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
        .options = &options_handler,
    };
    app = rg_system_init(AUDIO_SAMPLE_RATE, &handlers, NULL);
    // rg_system_set_overclock(2);

    // JIT feasibility test (jit_test.c): executable PSRAM via dual-mapping.
    // Re-enable only needed after IDF upgrades or new hardware:
    // jit_test_run();

    // Xtensa JIT pipeline self-test. Re-enable after emitter changes:
    // jit_selftest_run();

    // GBA needs all 4 hw buttons: brightness (X/Y) = GBA L, volume (L/R) = GBA R
    rg_input_set_gamepad_passthrough(RG_KEY_X | RG_KEY_Y | RG_KEY_L | RG_KEY_R);

    sound_master_enable = rg_settings_get_number(NS_APP, SETTING_SOUND_EMULATION, true);
    user_frameskip = rg_settings_get_number(NS_APP, SETTING_FRAMESKIP, 0);
    app->frameskip = user_frameskip;

    // Default scaling = Off (native 240x160, centered) on first run. The retro-go
    // global default is Full; we only override when the user hasn't picked a value
    // yet (sentinel < 0). After that rg_display_set_scaling persists the choice.
    if (rg_settings_get_number(NS_APP, "DispScaling", -1) < 0)
        rg_display_set_scaling(RG_DISPLAY_SCALING_OFF);

    // Xtensa JIT: translation cache in dual-mapped executable PSRAM. Off by default;
    // enable via Options → CPU core. On init failure: silently back to classic.
#ifdef ESP_PLATFORM
    {
        // 2MB: at 1MB (~3300 blocks of ~300B) the cache filled every 1-2s in busy
        // scenes, wiping everything -> recompile storms (~6-7% CPU + 100-300ms frame
        // hitches). Measured: flushes 45 -> 7 per 90s. The greedy ROM cache runs later
        // and takes what's left, so PSRAM rebalances itself (~1MB less ROM cache).
        const u32 jit_cache_size = 2 * 1024 * 1024;
        void *wr = heap_caps_aligned_alloc(0x10000, jit_cache_size, MALLOC_CAP_SPIRAM);
        void *hash = wr ? rg_alloc(JIT_HASH_BYTES, MEM_SLOW) : NULL;
        void *ex = NULL;
        if (wr && hash)
        {
            esp_paddr_t paddr; mmu_target_t tgt;
            if (esp_mmu_vaddr_to_paddr(wr, &paddr, &tgt) == ESP_OK &&
                esp_mmu_map(paddr, jit_cache_size, MMU_TARGET_PSRAM0,
                            MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ | MMU_MEM_CAP_32BIT,
                            ESP_MMU_MMAP_FLAG_PADDR_SHARED, &ex) == ESP_OK &&
                jit_init(wr, ex, jit_cache_size, hash) == 0)
            {
                jit_enabled = rg_settings_get_number(NS_APP, SETTING_JIT, JIT_DEFAULT);
                RG_LOGI("JIT cache: write=%p exec=%p size=%u", wr, ex, (unsigned)jit_cache_size);

                // Link-inline cache (see JIT_LINK_CACHE in cpu_jit.c). Small + guarded:
                // 4096 slots x 8B = 32KB PSRAM data. NULL → cpu_jit falls back to the
                // slow (hash) path.
                {
                    const u32 jlc_slots = 4096;            // hot set ~256 sites; ample, overflow = safe
                    void *jlc = rg_alloc(jlc_slots * 8, MEM_SLOW);   // jlc_t = {u32 gen; void* addr}
                    jit_set_link_cache(jlc, jlc ? jlc_slots : 0);
                    RG_LOGI("JIT link-cache: %s (%u slots)", jlc ? "on" : "alloc failed -> slow path",
                            (unsigned)(jlc ? jlc_slots : 0));
                }

                // Hot-block IRAM pinning: grab whatever executable internal RAM is free
                // (direct SRAM, no I-cache → no PSRAM fetch stalls).
                size_t exfree = heap_caps_get_largest_free_block(MALLOC_CAP_EXEC);
                size_t extot  = heap_caps_get_free_size(MALLOC_CAP_EXEC);
                size_t infree = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                // Pak wat exec-IRAM er is (2KB headroom), cap 24KB.
                u32 iram_sz = exfree > 3072 ? ((u32)exfree - 2048) & ~15u : 0;
                if (iram_sz > 24 * 1024) iram_sz = 24 * 1024;
                void *ib = iram_sz ? heap_caps_malloc(iram_sz, MALLOC_CAP_EXEC) : NULL;
                if (ib) jit_set_iram(ib, iram_sz);
                printf("[JIT] exec-free=%u (largest) exec-total=%u internal-free=%u -> IRAM-pin=%u @ %p\n",
                       (unsigned)exfree, (unsigned)extot, (unsigned)infree,
                       (unsigned)(ib ? iram_sz : 0), ib);
            }
            else
                RG_LOGW("JIT unavailable (mmu_map/init failed)");
        }
        else
            RG_LOGW("JIT unavailable (alloc failed)");
    }
#endif

    // The PPU sprite list explicitly goes to PSRAM: it used to be 100KB of
    // static BSS, which left no internal SRAM for iwram/vram below.
    gbsp_obj_priority_list = rg_alloc(sizeof(*gbsp_obj_priority_list), MEM_SLOW);

    // iwram and vram live in static BSS: internal SRAM, reserved at link time.
    // A runtime MEM_FAST allocation proved unreliable for vram: by the time
    // app_main runs, boot-time allocations have fragmented the internal heap
    // and no contiguous 96KB block is left (free internal can be fragmented
    // while vram still falls back to PSRAM).
    static u8 iwram_static[sizeof(iwram)];
    static u8 vram_static[sizeof(vram)];
    gbsp_iwram = (void *)&iwram_static;
    gbsp_vram = (void *)&vram_static;

    updates[0] = rg_surface_create(GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT + 1, RG_PIXEL_565_LE, MEM_FAST);
    updates[0]->height = GBA_SCREEN_HEIGHT;
    updates[1] = rg_surface_create(GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT + 1, RG_PIXEL_565_LE, MEM_FAST);
    if (!updates[1]) // Fallback to single-buffered if not enough internal RAM
    {
        updates[1] = updates[0];
    }
    else
    {
        updates[1]->height = GBA_SCREEN_HEIGHT;
    }
    currentUpdate = updates[0];

    gba_screen_pixels = currentUpdate->data;

    gbsp_memory = rg_alloc(sizeof(*gbsp_memory), MEM_ANY);
    // 0x3FC… = internal SRAM, 0x3C… = PSRAM
    RG_LOGI("gbsp_memory=%p iwram=%p vram=%p objlist=%p",
            gbsp_memory, gbsp_iwram, gbsp_vram, gbsp_obj_priority_list);

    libretro_supports_bitmasks = true;
    retro_set_input_state(input_cb);
    init_gamepak_buffer();
#if RENDER_OFFLOAD
    // Allocate the offload snapshot after the ROM cache (which leaves a margin).
    if (!render_offload_init())
        RG_LOGE("render_offload_init: snapshot alloc failed — offload falls back to core1");
    else
        render_offload_start();   // start the core0 render task
#endif
    init_sound();
    /* Real GBA BIOS auto-detect (distribution-safe): use a user-placed
     * /sd/BlockBoy/bios/gba_bios.bin if valid, otherwise fall back to the built-in
     * open-source BIOS. Do NOT bundle the real BIOS (Nintendo copyright).
     * Pokémon R/S/E are BIOS-sensitive (SWI/IRQ timing). */
    load_bios(RG_BASE_PATH_BIOS "/gba_bios.bin");

    memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
    /* RTC: FEAT_AUTODETECT -> per-game override (gba_over.h) decides; Ruby/Saph/Emer get RTC.
     * Needed for time-based events. */
    // Serial/link disabled: the GBA Wireless Adapter (RFU) link is blocked by the
    // internal-RAM budget (WiFi static .bss ~40KB doesn't fit; the GBA core fills
    // internal SRAM). Set to SERIAL_MODE_AUTO + GBA_LINK_NETLINK=1 once that is solved.
    if (load_gamepak(NULL, app->romPath, FEAT_AUTODETECT, FEAT_DISABLE, SERIAL_MODE_DISABLED) != 0)
    {
        RG_PANIC("Could not load the game file.");
    }


    RG_LOGI("reset_gba");
    reset_gba();

    bool resumed = false;
    if (app->bootFlags & RG_BOOT_RESUME)
    {
        RG_LOGI("load_state");
        resumed = rg_emu_load_state(app->saveSlot);
    }

    /* New game (no resume) → boot in classic, JIT on once the game renders.
     * Also on a FAILED resume (corrupt/missing savestate): the game then still runs
     * from the cold reset_gba() state = the same boot-poll hang risk. */
    if (!resumed)
        jit_defer_boot();

    // Clear the border once at game start. With scaling=Off the emulator only updates
    // the 240x160 viewport, so leftover pixels from the loading screen would otherwise
    // remain in the black border until the next display change (e.g. opening the menu).
    rg_display_force_redraw();

#if defined(ESP_PLATFORM) && GBA_LINK_NETLINK
    if (serial_mode == SERIAL_MODE_RFU)
        netlink_start();
#endif

    RG_LOGI("emulation loop");

    rg_audio_sample_t mixbuffer[AUDIO_BUFFER_LENGTH] = {0};

    while (true)
    {
        const int64_t startTime = rg_system_timer();
        uint32_t joystick = rg_input_read_gamepad();

        if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
        {
#if RENDER_OFFLOAD
            // Quiesce the offload pipeline before the menu, while the display is still free.
            // Otherwise the render task (core1) hangs mid-rg_display_submit once the menu
            // takes over the display → never returns s_snap_free → app_main blocks on
            // resume. Wait for the render task (timeout = safety net) and give s_snap_free.
            xSemaphoreTake(s_snap_free, pdMS_TO_TICKS(500));
            xSemaphoreGive(s_snap_free);
#endif
            if (joystick & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
            memset(&mixbuffer, 0, sizeof(mixbuffer));
            // Clear the border after the menu (e.g. after loading a save state, which
            // can leave the green system background in the border when scaling=Off).
            rg_display_force_redraw();
            continue;
        }


        /* Once boot is past (game renders = DISPCNT forced-blank off), enable JIT.
         * Min. 30 frames against a transient initial clear; hard cap 600 as a safety net. */
        if (jit_boot_defer)
        {
            jit_boot_frames++;
            int rendering = !(read_ioreg(REG_DISPCNT) & 0x80);
            if ((rendering && jit_boot_frames >= 30) || jit_boot_frames >= 600)
            {
                jit_invalidate_all();
                jit_enabled = 1;
                jit_boot_defer = 0;
            }
        }

        update_input();
        rumble_frame_reset();
        clear_gamepak_stickybits();
#if RENDER_OFFLOAD
        // Offload only at normal speed without frameskip. The offload sync throttles
        // core0 to blit speed; on fast-forward or frameskip core0 wants to go faster →
        // fall back to live render on core0.
        g_offload_active = (rg_emu_get_speed() <= 1.01f && user_frameskip < 0);
        // Active→off transition = drain the pipeline once (wait for the render task) so
        // the OFF path afterwards has no semaphore interaction.
        if (s_offload_was_active && !g_offload_active)
        {
            xSemaphoreTake(s_snap_free, pdMS_TO_TICKS(500));
            xSemaphoreGive(s_snap_free);
        }
        s_offload_was_active = g_offload_active;
        // Scratch race: only on an active dirty frame does core0 render live while
        // core1 may still render a previous offload frame → wait first. (Active+clean =
        // core0 doesn't render; inactive = render task idle → no sync needed.)
        if (g_offload_active && !g_predict_clean)
        {
            xSemaphoreTake(s_snap_free, portMAX_DELAY);
            xSemaphoreGive(s_snap_free);
        }
#endif
        execute_jit(execute_cycles); // falls back to execute_arm in Classic mode

        // Wireless Adapter (RFU) per-frame housekeeping (broadcast announce timing,
        // timeouts). Only active for FLAGS_RFU games; no-op otherwise. (rfu_update is
        // already driven via update_serial in the core loop.)
        if (serial_mode == SERIAL_MODE_RFU)
            rfu_frame_update();

        if (!skip_next_frame)
        {
#if RENDER_OFFLOAD
            // Was this frame predicted clean? Then the emu core skipped the render
            // -> snapshot + let the render core draw+blit it. Otherwise rendered live
            // -> submit it here (with transition sync against frame reorder).
            bool was_clean = (g_offload_active && g_predict_clean);
            if (was_clean)
            {
                xSemaphoreTake(s_snap_free, portMAX_DELAY);  // render task done with g_snap
                render_snap_capture();                       // live state -> g_snap (emu core)
                s_render_target = currentUpdate;
                xSemaphoreGive(s_snap_ready);                // wake the render task
            }
            else
            {
                // offload off, or dirty: core0 rendered live → submit it here.
                // (the pre-execute_jit sync kept core0 and the render task separated.)
                rg_display_submit(currentUpdate, 0);
            }
            currentUpdate = (currentUpdate == updates[0]) ? updates[1] : updates[0];
            gba_screen_pixels = currentUpdate->data;
            g_predict_clean = (g_m2_dirty == 0);             // predict the next frame
            g_m2_dirty = 0;
#else
            rg_display_submit(currentUpdate, 0);
            currentUpdate = (currentUpdate == updates[0]) ? updates[1] : updates[0];
            gba_screen_pixels = currentUpdate->data;
#endif
        }

        rg_system_tick(rg_system_timer() - startTime);

        // Decide skips before rg_audio_submit: that call blocks for pacing and
        // would inflate the elapsed measurement used by auto frameskip.
        int64_t elapsed = rg_system_timer() - startTime;

        if (skip_next_frame == 0)
        {
            if (user_frameskip > 0)                    // fixed 1-9
                skip_next_frame = user_frameskip;
            else if (user_frameskip == 0 && elapsed > app->frameTime + 1500) // Auto: skip only when behind
                skip_next_frame = RG_MIN(elapsed / (int64_t)app->frameTime, 5);
            // user_frameskip < 0 = Off: never skip (render every frame)
        }
        else if (skip_next_frame > 0)
            skip_next_frame--;

        if (sound_master_enable)
        {
            size_t frames_count = sound_read_samples((s16 *)mixbuffer, AUDIO_BUFFER_LENGTH);
            rg_audio_submit(mixbuffer, frames_count);
        }
        else if (elapsed < app->frameTime)
        {
            // No audio = no pacing from rg_audio_submit; don't run faster than 60fps
            rg_task_delay((app->frameTime - elapsed) / 1000);
        }
    }

    RG_PANIC("GBsP Ended");
}
