#include "shared.h"

#include <sys/time.h>
#include <gnuboy.h>

#ifdef RG_ENABLE_NETPLAY
#include <rg_netplay.h>
#include <esp_wifi.h>

// The emulator core doesn't know rg_netplay.h, so both sides define their own
// frame types. They do have to share the same values.
_Static_assert(GB_LINK_MASTER == RG_LINK_MASTER, "Game Link frame types diverged");
_Static_assert(GB_LINK_REPLY  == RG_LINK_REPLY,  "Game Link frame types diverged");

static void gb_wifi_send(uint8_t type, uint8_t seq, uint8_t data)
{
    rg_netplay_link_send(type, seq, data);
}

static bool gb_wifi_recv(uint8_t *type, uint8_t *seq, uint8_t *data)
{
    return rg_netplay_link_recv(type, seq, data);
}

// In-game status message: set from gamelink_callback (netplay task, Core 1),
// drawn from the main loop (Core 0). Set linkMsgUntil first, then linkMsg.
// linkMsg holds the untranslated text; _() is applied at draw time so the
// pointer stays stable (we compare it against linkMsgShown).
static const char *volatile linkMsg = NULL;
static volatile int64_t linkMsgUntil = 0;
static const char *linkMsgShown = NULL;
static bool linkWasConnected = false;

static void gamelink_callback(netplay_event_t event, void *arg)
{
    if ((event & 0xFF) != NETPLAY_EVENT_STATUS_CHANGED)
        return;

    netplay_status_t status = rg_netplay_status();

    if (status == NETPLAY_STATUS_CONNECTED)
    {
        RG_LOGI("Game Link connected\n");
        gnuboy_set_serial_callback(&gb_wifi_send, &gb_wifi_recv);
        gnuboy_set_serial_disconnect_cb(&rg_netplay_close_serial);
        linkWasConnected = true;
        linkMsgUntil = rg_system_timer() + 2000000;
        linkMsg = "Game Link connected";
    }
    else if (status == NETPLAY_STATUS_LISTENING || status == NETPLAY_STATUS_HANDSHAKE)
    {
        gnuboy_set_no_partner(false);
    }
    else if (status == NETPLAY_STATUS_DISCONNECTED || status == NETPLAY_STATUS_STOPPED)
    {
        RG_LOGI("Game Link disconnected\n");
        gnuboy_set_serial_callback(NULL, NULL);
        // Only report when an active session actually dropped — STOPPED also
        // passes by on a cancelled connection attempt in the menu.
        if (linkWasConnected)
        {
            linkWasConnected = false;
            linkMsgUntil = rg_system_timer() + 4000000;
            linkMsg = "Game Link disconnected!";
        }
    }
}
#endif

static int skipFrames = 0;
static bool slowFrame = false;

static int video_time;
static int audio_time;

static const char *sramFile;
static int autoSaveSRAM = 0;
static int autoSaveSRAM_Timer = 0;
static bool useSystemTime = true;
static bool loadBIOSFile = false;

static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;

static const char *SETTING_SAVESRAM = "SaveSRAM";
static const char *SETTING_PALETTE  = "Palette";
static const char *SETTING_SYSTIME = "SysTime";
static const char *SETTING_LOADBIOS = "LoadBIOS";
// --- MAIN


static void update_rtc_time(void)
{
    if (!useSystemTime)
        return;
    time_t timer = time(NULL);
    struct tm *info = localtime(&timer);
    gnuboy_set_time(info->tm_yday, info->tm_hour, info->tm_min, info->tm_sec);
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(currentUpdate, 0);
    }
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    return gnuboy_save_state(filename) == 0;
}

static bool load_state_handler(const char *filename)
{
    if (gnuboy_load_state(filename) != 0)
    {
        // If a state fails to load then we should behave as we do on boot
        // which is a hard reset and load sram if present
        gnuboy_reset(true);
        gnuboy_load_sram(sramFile);
        update_rtc_time();

        return false;
    }

    update_rtc_time();

    skipFrames = 0;
    autoSaveSRAM_Timer = 0;

    // TO DO: Call rtc_sync() if a physical RTC is present
    return true;
}

static bool reset_handler(bool hard)
{
    gnuboy_reset(hard);
    update_rtc_time();

    skipFrames = 0;
    autoSaveSRAM_Timer = 0;

    return true;
}

static rg_gui_event_t palette_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (gnuboy_get_hwtype() == GB_HW_CGB)
    {
        strcpy(option->value, "GBC");
        return RG_DIALOG_VOID;
    }

    int pal = gnuboy_get_palette();
    int max = GB_PALETTE_COUNT - 1;

    if (event == RG_DIALOG_PREV)
        pal = pal > 0 ? pal - 1 : max;

    if (event == RG_DIALOG_NEXT)
        pal = pal < max ? pal + 1 : 0;

    if (pal != gnuboy_get_palette())
    {
        rg_settings_set_number(NS_APP, SETTING_PALETTE, pal);
        gnuboy_set_palette(pal);
        gnuboy_run(true);
        return RG_DIALOG_REDRAW;
    }

    if (pal == GB_PALETTE_DMG)
        strcpy(option->value, "DMG   ");
    else if (pal == GB_PALETTE_MGB0)
        strcpy(option->value, "Pocket");
    else if (pal == GB_PALETTE_MGB1)
        strcpy(option->value, "Light ");
    else if (pal == GB_PALETTE_CGB)
        strcpy(option->value, "GBC   ");
    else if (pal == GB_PALETTE_SGB)
        strcpy(option->value, "SGB   ");
    else
        sprintf(option->value, "%d/%d   ", pal + 1, max - 1);

    return RG_DIALOG_VOID;
}

static rg_gui_event_t sram_autosave_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV) autoSaveSRAM--;
    if (event == RG_DIALOG_NEXT) autoSaveSRAM++;

    autoSaveSRAM = RG_MIN(RG_MAX(0, autoSaveSRAM), 999);

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        rg_settings_set_number(NS_APP, SETTING_SAVESRAM, autoSaveSRAM);
    }

    if (autoSaveSRAM == 0) strcpy(option->value, _("Off"));
    else sprintf(option->value, "%3ds", autoSaveSRAM);

    return RG_DIALOG_VOID;
}

static rg_gui_event_t enable_bios_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        loadBIOSFile = !loadBIOSFile;
        rg_settings_set_number(NS_APP, SETTING_LOADBIOS, loadBIOSFile);
    }
    strcpy(option->value, loadBIOSFile ? _("Yes") : _("No"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t rtc_t_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int d, h, m, s;

    gnuboy_get_time(&d, &h, &m, &s);

    if (option->arg == 'd') {
        if (event == RG_DIALOG_PREV && --d < 0) d = 364;
        if (event == RG_DIALOG_NEXT && ++d > 364) d = 0;
        sprintf(option->value, "%02d", d);
    }
    if (option->arg == 'h') {
        if (event == RG_DIALOG_PREV && --h < 0) h = 23;
        if (event == RG_DIALOG_NEXT && ++h > 23) h = 0;
        sprintf(option->value, "%02d", h);
    }
    if (option->arg == 'm') {
        if (event == RG_DIALOG_PREV && --m < 0) m = 59;
        if (event == RG_DIALOG_NEXT && ++m > 59) m = 0;
        sprintf(option->value, "%02d", m);
    }
    if (option->arg == 's') {
        if (event == RG_DIALOG_PREV && --s < 0) s = 59;
        if (event == RG_DIALOG_NEXT && ++s > 59) s = 0;
        sprintf(option->value, "%02d", s);
    }
    if (option->arg == 'x') {
        if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
            useSystemTime = !useSystemTime;
            rg_settings_set_number(NS_APP, SETTING_SYSTIME, useSystemTime);
        }
        strcpy(option->value, useSystemTime ? _("Yes") : _("No"));
    }

    gnuboy_set_time(d, h, m, s);

    // TO DO: Update system clock

    return RG_DIALOG_VOID;
}

static rg_gui_event_t rtc_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER) {
        const rg_gui_option_t choices[] = {
            {'d', _("Day"),  "-", RG_DIALOG_FLAG_NORMAL, &rtc_t_update_cb},
            {'h', _("Hour"), "-", RG_DIALOG_FLAG_NORMAL, &rtc_t_update_cb},
            {'m', _("Min"),  "-", RG_DIALOG_FLAG_NORMAL, &rtc_t_update_cb},
            {'s', _("Sec"),  "-", RG_DIALOG_FLAG_NORMAL, &rtc_t_update_cb},
            {'x', _("Sync"), "-", RG_DIALOG_FLAG_NORMAL, &rtc_t_update_cb},
            RG_DIALOG_END
        };
        rg_gui_dialog(option->label, choices, 0);
    }
    int h, m;
    gnuboy_get_time(NULL, &h, &m, NULL);
    sprintf(option->value, "%02d:%02d", h, m);
    return RG_DIALOG_VOID;
}

static void video_callback(void *buffer)
{
    int64_t startTime = rg_system_timer();
    slowFrame = !rg_display_sync(false);
    rg_display_submit(currentUpdate, 0);
    video_time += rg_system_timer() - startTime;
}


static void audio_callback(void *buffer, size_t length)
{
    int64_t startTime = rg_system_timer();
    rg_audio_submit(buffer, length >> 1);
    audio_time += rg_system_timer() - startTime;
}

static void set_showcase_rom_name(const char *path)
{
    rg_gui_set_showcase_game_name(path);
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Palette"),       "-", RG_DIALOG_FLAG_NORMAL, &palette_update_cb};
    *dest++ = (rg_gui_option_t){0, _("RTC config"),    "-", RG_DIALOG_FLAG_NORMAL, &rtc_update_cb};
    *dest++ = (rg_gui_option_t){0, _("SRAM autosave"), "-", RG_DIALOG_FLAG_NORMAL, &sram_autosave_cb};
    *dest++ = (rg_gui_option_t){0, _("Enable BIOS"),   "-", RG_DIALOG_FLAG_NORMAL, &enable_bios_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

// Pre-shuffled showcase rotation written by the launcher (see SHOWCASE_GUIDE.md).
#define SHOWCASE_ROTATION_FILE RG_BASE_PATH_CONFIG "/showcase_rotation.txt"

// Unzip buffer for the current ROM (zip case). gnuboy_load_rom() references this
// buffer directly, so we keep it and free it ourselves on the next load. NULL when
// the ROM was loaded straight from a file (gnuboy_free_rom frees those banks itself).
static void *romData = NULL;

// (Re)loads a GB/GBC ROM into the running emulator, freeing the previous one first.
static bool load_game(const char *path)
{
    gnuboy_free_rom(); // frees previous ROM banks/file; safe on the very first call
    if (romData) { free(romData); romData = NULL; }

    if (rg_extension_match(path, "zip"))
    {
        void *data;
        size_t size;
        if (!rg_storage_unzip_file(path, NULL, &data, &size, RG_FILE_ALIGN_16KB))
            return false;
        if (gnuboy_load_rom(data, size) < 0) { free(data); return false; }
        romData = data; // gnuboy references it; freed on the next load_game()
        return true;
    }
    return gnuboy_load_rom_file(path) >= 0;
}

// Showcase in-app switch: load the next ROM from the rotation cache WITHOUT rebooting.
// Returns false when it can't (cache missing, or the rotation wrapped) so the caller
// reboots to the launcher, which reshuffles and relaunches.
static bool showcase_load_next(void)
{
    int index = rg_settings_get_number(NS_GLOBAL, "DisplayIndex", 0);

    FILE *fp = fopen(SHOWCASE_ROTATION_FILE, "r");
    if (!fp)
        return false;

    char line[RG_PATH_MAX + 16];
    char rom[RG_PATH_MAX + 1] = {0};
    bool found = false;
    int count = 0;
    while (fgets(line, sizeof(line), fp))
    {
        if (count == index)
        {
            // Each line is "short_name\tfolder/name".
            char *tab = strchr(line, '\t');
            if (tab)
            {
                char *p = tab + 1;
                p[strcspn(p, "\r\n")] = 0;
                strncpy(rom, p, RG_PATH_MAX);
                found = (rom[0] != 0);
            }
        }
        count++;
    }
    fclose(fp);

    if (!found || index >= count)
        return false; // wrapped or bad entry -> let the launcher reshuffle

    if (gnuboy_sram_dirty()) // keep the current game's save
        gnuboy_save_sram(sramFile, false);

    if (!load_game(rom))
        return false;

    set_showcase_rom_name(rom);
    free((void *)sramFile); // rg_emu_get_path malloc's, so free the previous one
    sramFile = rg_emu_get_path(RG_PATH_SAVE_SRAM, rom);
    rg_storage_mkdir(rg_dirname(sramFile));
    gnuboy_set_palette(rg_settings_get_number(NS_APP, SETTING_PALETTE, GB_PALETTE_DMG));
    gnuboy_reset(true);
    gnuboy_load_sram(sramFile);
    update_rtc_time();
    skipFrames = 0;
    autoSaveSRAM_Timer = 0;

    rg_settings_set_number(NS_GLOBAL, "DisplayIndex", index + 1);
    rg_settings_commit();
    return true;
}

void gbc_main(void)
{
    const rg_handlers_t handlers = {
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
        .options = &options_handler,
    };

    app = rg_system_reinit(AUDIO_SAMPLE_RATE, &handlers, NULL);

    updates[0] = rg_surface_create(GB_WIDTH, GB_HEIGHT, RG_PIXEL_565_BE, MEM_ANY);
    updates[1] = rg_surface_create(GB_WIDTH, GB_HEIGHT, RG_PIXEL_565_BE, MEM_ANY);
    currentUpdate = updates[0];

    useSystemTime = (bool)rg_settings_get_number(NS_APP, SETTING_SYSTIME, 1);
    loadBIOSFile = (bool)rg_settings_get_number(NS_APP, SETTING_LOADBIOS, 0);
    autoSaveSRAM = (int)rg_settings_get_number(NS_APP, SETTING_SAVESRAM, 0);
    sramFile = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);

    if (!rg_storage_mkdir(rg_dirname(sramFile)))
        RG_LOGE("Unable to create SRAM folder...");

    // Initialize the emulator
    if (gnuboy_init(app->sampleRate, GB_AUDIO_STEREO_S16, GB_PIXEL_565_BE, &video_callback, &audio_callback) < 0)
        RG_PANIC("Emulator init failed!");

    gnuboy_set_framebuffer(currentUpdate->data);
    gnuboy_set_soundbuffer(malloc(AUDIO_BUFFER_LENGTH * 4), AUDIO_BUFFER_LENGTH);

    // Load ROM
    if (!load_game(app->romPath))
    {
        if (rg_settings_get_number(NS_GLOBAL, "DisplayActive", 0))
            rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0); // skip bad ROM, next showcase game
        RG_PANIC("ROM Loading failed!");
    }
    set_showcase_rom_name(app->romPath);

    // This GB/GBC emulator can switch showcase games in-place, so the monitor task
    // signals us instead of rebooting to the launcher between games (no flicker).
    rg_system_set_inapp_showcase(true);

    // Load BIOS
    if (loadBIOSFile)
    {
        if (gnuboy_get_hwtype() == GB_HW_CGB)
            gnuboy_load_bios_file(RG_BASE_PATH_BIOS "/gbc_bios.bin");
        else if (gnuboy_get_hwtype() == GB_HW_DMG)
            gnuboy_load_bios_file(RG_BASE_PATH_BIOS "/gb_bios.bin");
    }

    gnuboy_set_palette(rg_settings_get_number(NS_APP, SETTING_PALETTE, GB_PALETTE_DMG));

    // Hard reset to have a clean slate
    gnuboy_reset(true);

    // Load saved state or SRAM
    if (app->bootFlags & RG_BOOT_RESUME)
        rg_emu_load_state(app->saveSlot);
    else
        gnuboy_load_sram(sramFile);

    update_rtc_time();

    // Auto frameskip. rg_system's default is a *fixed* frameskip of 1, which makes
    // the loop below drop every second frame unconditionally -- so the Game Boy ran
    // at 30 drawn fps no matter how much headroom there was, and scrolling games
    // (Super Mario Land) looked like they were struggling. 0 means "only skip when a
    // frame actually ran long", which is what the elapsed/slowFrame checks in the
    // loop are for. main_sms.c already does exactly this.
    app->frameskip = 0;

#ifdef RG_ENABLE_NETPLAY
    rg_netplay_init(&gamelink_callback);
#endif

    // Ready!

    uint32_t joystick_old = -1;
    uint32_t joystick = 0;

    while (true)
    {
        // Showcase: the per-game timer elapsed — load the next ROM in-place (no reboot).
        // On a wrap (or any failure) fall back to the launcher, which reshuffles. The
        // DisplayActive check ignores a switch that was queued just before the user
        // picked "Play this game" in the menu (which clears DisplayActive).
        if (rg_system_showcase_next_pending() && rg_settings_get_number(NS_GLOBAL, "DisplayActive", 0))
        {
            if (!showcase_load_next())
                rg_system_switch_app(RG_APP_LAUNCHER, NULL, NULL, 0);
            joystick_old = -1; // new game: force a fresh pad state next iteration
            continue;
        }

        joystick = rg_input_read_gamepad();

        if (joystick & (RG_KEY_MENU|RG_KEY_OPTION))
        {
            if (joystick & RG_KEY_MENU)
            {
                if (gnuboy_sram_dirty()) // save in case the user quits
                    gnuboy_save_sram(sramFile, false);
                rg_gui_game_menu();
            }
            else
                rg_gui_options_menu();
        }
        else if (joystick != joystick_old)
        {
            int pad = 0;
            if (joystick & RG_KEY_UP) pad |= GB_PAD_UP;
            if (joystick & RG_KEY_RIGHT) pad |= GB_PAD_RIGHT;
            if (joystick & RG_KEY_DOWN) pad |= GB_PAD_DOWN;
            if (joystick & RG_KEY_LEFT) pad |= GB_PAD_LEFT;
            if (joystick & RG_KEY_SELECT) pad |= GB_PAD_SELECT;
            if (joystick & RG_KEY_START) pad |= GB_PAD_START;
            if (joystick & RG_KEY_A) pad |= GB_PAD_A;
            if (joystick & RG_KEY_B) pad |= GB_PAD_B;
            gnuboy_set_pad(pad); // That call is somewhat costly, that's why we try to avoid it
            joystick_old = joystick;
        }

        int64_t startTime = rg_system_timer();
        bool drawFrame = !skipFrames;

#ifdef RG_ENABLE_NETPLAY
        // While the link popup is up we keep emulating (the partner waits on
        // the serial link) but stop sending frames to the screen, otherwise the
        // popup flickers away under every new game frame.
        if (linkMsg)
            drawFrame = false;
#endif

        video_time = audio_time = 0;

        if (drawFrame)
        {
            currentUpdate = updates[currentUpdate == updates[0]];
            gnuboy_set_framebuffer(currentUpdate->data);
        }
        gnuboy_run(drawFrame);

#ifdef RG_ENABLE_NETPLAY
        if (rg_netplay_status() == NETPLAY_STATUS_CONNECTED) {
            int64_t target   = startTime + (int64_t)app->frameTime;
            int64_t now      = rg_system_timer();
            if (target > now) {
                int64_t yield_us = target - now;
                if (yield_us > 1000)
                    rg_task_delay((uint32_t)(yield_us / 1000));
            }
            while (rg_system_timer() < target) ;
        }

        // Link status message as a regular popup (same style/position as the
        // rest of the GUI). Draw once while it is up; afterwards restore the
        // game image via the same path as closing the menu.
        const char *msg = linkMsg;
        if (msg) {
            if (rg_system_timer() < linkMsgUntil) {
                if (msg != linkMsgShown) {
                    rg_display_sync(true);
                    rg_gui_draw_message("%s", _(msg));
                    linkMsgShown = msg;
                }
            } else {
                linkMsg = NULL;
                linkMsgShown = NULL;
                rg_system_event(RG_EVENT_REDRAW, NULL);
            }
        }
#endif

        if (autoSaveSRAM > 0
#ifdef RG_ENABLE_NETPLAY
            && rg_netplay_status() != NETPLAY_STATUS_CONNECTED
#endif
        )
        {
            if (autoSaveSRAM_Timer <= 0)
            {
                if (gnuboy_sram_dirty())
                {
                    autoSaveSRAM_Timer = autoSaveSRAM * 60;
                }
            }
            else if (--autoSaveSRAM_Timer == 0)
            {
                gnuboy_save_sram(sramFile, true);
            }
        }

        // Tick before submitting audio/syncing
        rg_system_tick(rg_system_timer() - startTime - audio_time);

        if (skipFrames == 0)
        {
            int elapsed = rg_system_timer() - startTime;
            if (app->frameskip > 0)
                skipFrames = app->frameskip;
            else if (elapsed > app->frameTime + 1500) // Allow some jitter
                skipFrames = 1; // (elapsed / frameTime)
            else if (drawFrame && slowFrame)
                skipFrames = 1;
        }
        else if (skipFrames > 0)
        {
            skipFrames--;
        }
    }
}
