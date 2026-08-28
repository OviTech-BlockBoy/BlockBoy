#include <rg_system.h>
#include <sys/time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

#include "applications.h"
#include "bookmarks.h"
#include "browser.h"
#include "gui.h"
#include "webui.h"
#include "updater.h"
#include "usb_msc.h"

static rg_app_t *app;

#define SETTING_WEBUI "HTTPFileServer"

// Boot animation - BlockBoy logo scrolls down like Game Boy
// TODO: Not working yet - see boot_animation_notes.md
/*
static void boot_animation(void)
{
    int screen_width = rg_display_get_width();
    int screen_height = rg_display_get_height();
    int center_y = (screen_height / 2) - 10;

    // Animate "BlockBoy" scrolling down
    for (int y = -30; y <= center_y; y += 4)
    {
        rg_display_clear(C_WHITE);
        rg_gui_draw_text(0, y, screen_width, "BlockBoy", C_BLACK, C_WHITE, RG_TEXT_ALIGN_CENTER | RG_TEXT_BIGGER);
        rg_task_delay(15);
    }

    // Hold at center
    rg_task_delay(800);

    // Fade to black
    rg_display_clear(C_BLACK);
    rg_task_delay(300);
}
*/

static rg_gui_event_t toggle_tab_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    tab_t *tab = gui.tabs[option->arg];
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
    {
        tab->enabled = !tab->enabled;
    }
    strcpy(option->value, tab->enabled ? _("Show") : _("Hide"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t toggle_tabs_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        rg_gui_option_t options[gui.tabs_count + 1];
        rg_gui_option_t *opt = options;

        for (size_t i = 0; i < gui.tabs_count; ++i)
            *opt++ = (rg_gui_option_t){i, gui.tabs[i]->name, "...", 1, &toggle_tab_cb};
        *opt++ = (rg_gui_option_t)RG_DIALOG_END;

        rg_gui_dialog(option->label, options, 0);
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t scroll_mode_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    const char *modes[SCROLL_MODE_COUNT] = {_("Center"), _("Paging")};
    const int max = SCROLL_MODE_COUNT - 1;

    if (event == RG_DIALOG_PREV && --gui.scroll_mode < 0)
        gui.scroll_mode = max;
    if (event == RG_DIALOG_NEXT && ++gui.scroll_mode > max)
        gui.scroll_mode = 0;

    gui.scroll_mode %= SCROLL_MODE_COUNT;

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        return RG_DIALOG_REDRAW;

    strcpy(option->value, modes[gui.scroll_mode]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t start_screen_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    const char *modes[START_SCREEN_COUNT] = {_("Auto"), _("Carousel"), _("Browser")};
    const int max = START_SCREEN_COUNT - 1;

    if (event == RG_DIALOG_PREV && --gui.start_screen < 0)
        gui.start_screen = max;
    if (event == RG_DIALOG_NEXT && ++gui.start_screen > max)
        gui.start_screen = 0;

    gui.start_screen %= START_SCREEN_COUNT;

    strcpy(option->value, modes[gui.start_screen]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t show_preview_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    const char *modes[] = {_("None"), _("Cover,Save"), _("Save,Cover"), _("Cover only"), _("Save only")};
    const int max = PREVIEW_MODE_COUNT - 1;

    if (event == RG_DIALOG_PREV && --gui.show_preview < 0)
        gui.show_preview = max;
    if (event == RG_DIALOG_NEXT && ++gui.show_preview > max)
        gui.show_preview = 0;

    gui.show_preview %= PREVIEW_MODE_COUNT;

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        gui_set_preview(gui_get_current_tab(), NULL);
        if (gui.browse)
        {
            // Ugly hack otherwise gui_load_preview will abort...
            rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
            gui.joystick = 0;
            gui_load_preview(gui_get_current_tab());
        }
        return RG_DIALOG_REDRAW;
    }

    strcpy(option->value, modes[gui.show_preview]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t color_theme_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int max = RG_COUNT(gui.themes) - 1;

    if (event == RG_DIALOG_PREV && --gui.color_theme < 0)
        gui.color_theme = max;
    if (event == RG_DIALOG_NEXT && ++gui.color_theme > max)
        gui.color_theme = 0;

    gui.color_theme %= RG_COUNT(gui.themes);
    gui.theme = &gui.themes[gui.color_theme];

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        return RG_DIALOG_REDRAW;

    sprintf(option->value, "%d/%d", gui.color_theme + 1, max + 1);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t startup_app_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    const char *modes[] = {_("Last game"), _("Launcher")};
    int max = 1;

    if (event == RG_DIALOG_PREV && --gui.startup_mode < 0)
        gui.startup_mode = max;
    if (event == RG_DIALOG_NEXT && ++gui.startup_mode > max)
        gui.startup_mode = 0;

    strcpy(option->value, modes[gui.startup_mode % (max + 1)]);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t bootanim_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    const char *modes[BOOTANIM_COUNT] = {_("Blocks"), _("Scroll"), _("Off")};
    const int max = BOOTANIM_COUNT - 1;

    if (event == RG_DIALOG_PREV && --gui.bootanim < 0)
        gui.bootanim = max;
    if (event == RG_DIALOG_NEXT && ++gui.bootanim > max)
        gui.bootanim = 0;

    gui.bootanim %= BOOTANIM_COUNT;

    strcpy(option->value, modes[gui.bootanim]);
    return RG_DIALOG_VOID;
}

typedef struct { retro_app_t *app; int file_idx; } display_entry_t;

static void shuffle_entries(display_entry_t *entries, int count, uint32_t seed)
{
    for (int i = count - 1; i > 0; i--)
    {
        seed = seed * 1103515245 + 12345;
        int j = (seed >> 16) % (i + 1);
        display_entry_t temp = entries[i];
        entries[i] = entries[j];
        entries[j] = temp;
    }
}

static rg_gui_event_t display_mode_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    const char *modes[DISPLAY_MODE_COUNT] = {_("Off"), "GB", "GBC", _("Shuffle")};
    const int max = DISPLAY_MODE_COUNT - 1;

    if (event == RG_DIALOG_PREV && --gui.display_mode < 0)
        gui.display_mode = max;
    if (event == RG_DIALOG_NEXT && ++gui.display_mode > max)
        gui.display_mode = 0;

    gui.display_mode %= DISPLAY_MODE_COUNT;

    if (event == RG_DIALOG_ENTER && gui.display_mode != DISPLAY_MODE_OFF)
    {
        // Init apps and count available games
        retro_app_t *gb_app = application_find("gb");
        retro_app_t *gbc_app = application_find("gbc");
        int total_files = 0;

        if ((gui.display_mode == DISPLAY_MODE_GB || gui.display_mode == DISPLAY_MODE_SHUFFLE) && gb_app)
        {
            application_start_by_index(gb_app, -1); // Init only (index -1 returns early)
            for (size_t i = 0; i < gb_app->files_count; i++)
                if (gb_app->files[i].type == RETRO_TYPE_FILE)
                    total_files++;
        }
        if ((gui.display_mode == DISPLAY_MODE_GBC || gui.display_mode == DISPLAY_MODE_SHUFFLE) && gbc_app)
        {
            application_start_by_index(gbc_app, -1); // Init only
            for (size_t i = 0; i < gbc_app->files_count; i++)
                if (gbc_app->files[i].type == RETRO_TYPE_FILE)
                    total_files++;
        }

        if (total_files == 0)
        {
            rg_gui_alert(_("Showcase mode"), _("No games found!"));
            return RG_DIALOG_VOID;
        }

        // Build sorted entries list
        display_entry_t *entries = calloc(total_files, sizeof(display_entry_t));
        int idx = 0;
        if ((gui.display_mode == DISPLAY_MODE_GB || gui.display_mode == DISPLAY_MODE_SHUFFLE) && gb_app)
            for (size_t i = 0; i < gb_app->files_count; i++)
                if (gb_app->files[i].type == RETRO_TYPE_FILE)
                    entries[idx++] = (display_entry_t){gb_app, i};
        if ((gui.display_mode == DISPLAY_MODE_GBC || gui.display_mode == DISPLAY_MODE_SHUFFLE) && gbc_app)
            for (size_t i = 0; i < gbc_app->files_count; i++)
                if (gbc_app->files[i].type == RETRO_TYPE_FILE)
                    entries[idx++] = (display_entry_t){gbc_app, i};

        // Generate random seed and shuffle the list
        uint32_t seed = (uint32_t)rg_system_timer();
        shuffle_entries(entries, total_files, seed);

        // Activate display mode - index 0 will be launched, next cycle starts at 1
        rg_settings_set_number(NS_GLOBAL, "DisplayActive", 1);
        rg_settings_set_number(NS_GLOBAL, "DisplayFilter", gui.display_mode);
        rg_settings_set_number(NS_GLOBAL, "DisplayIndex", 1);
        rg_settings_set_number(NS_GLOBAL, "DisplaySeed", seed);
        gui_save_config();
        rg_settings_commit();

        // Launch first game from the shuffled list
        application_start_by_index(entries[0].app, entries[0].file_idx);
        free(entries); // Won't reach here
    }

    strcpy(option->value, modes[gui.display_mode]);
    return RG_DIALOG_VOID;
}

#if RG_USB_MSC_SUPPORTED
static rg_gui_event_t usb_mode_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        usb_msc_run_with_ui();
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}
#endif

#ifdef RG_ENABLE_NETWORKING
static rg_gui_event_t updater_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (rg_network_get_info().state != RG_NETWORK_CONNECTED)
    {
        option->flags = RG_DIALOG_FLAG_DISABLED;
        return RG_DIALOG_VOID;
    }
    if (event == RG_DIALOG_ENTER)
    {
        updater_show_dialog();
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t webui_switch_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    bool enabled = rg_settings_get_number(NS_APP, SETTING_WEBUI, 0);
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
    {
        enabled = !enabled;
        webui_stop();
        if (enabled)
            webui_start();
        rg_settings_set_number(NS_APP, SETTING_WEBUI, enabled);
    }
    strcpy(option->value, enabled ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}
#endif

static rg_gui_event_t prebuild_cache_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        rg_input_wait_for_key(RG_KEY_ANY, false, 1000);
        #ifdef RG_ENABLE_NETWORKING
        webui_stop();
        #endif
        crc_cache_prebuild();
    }
    return RG_DIALOG_VOID;
}

static void retro_loop(void)
{
    tab_t *tab = NULL;
    int64_t next_repeat = 0;
    int64_t next_idle_event = 0;
    int repeats = 0;
    int joystick, prev_joystick;
    int change_tab = 0;
    int browse_last = -1;
    bool redraw_pending = true;

    gui_init(app->isColdBoot);
    applications_init();
    bookmarks_init();
    // browser_init();

    // Display mode auto-launch: if active, launch the next game immediately
    if (rg_settings_get_number(NS_GLOBAL, "DisplayActive", 0))
    {
        int filter = rg_settings_get_number(NS_GLOBAL, "DisplayFilter", DISPLAY_MODE_GB);
        int index = rg_settings_get_number(NS_GLOBAL, "DisplayIndex", 0);

        // Build combined file list based on filter
        retro_app_t *gb_app = application_find("gb");
        retro_app_t *gbc_app = application_find("gbc");

        display_entry_t *entries = NULL;
        int total = 0;

        // Count total files first
        if ((filter == DISPLAY_MODE_GB || filter == DISPLAY_MODE_SHUFFLE) && gb_app)
        {
            application_start_by_index(gb_app, -1); // Trigger init if needed
            for (size_t i = 0; i < gb_app->files_count; i++)
                if (gb_app->files[i].type == RETRO_TYPE_FILE)
                    total++;
        }
        if ((filter == DISPLAY_MODE_GBC || filter == DISPLAY_MODE_SHUFFLE) && gbc_app)
        {
            application_start_by_index(gbc_app, -1); // Trigger init if needed
            for (size_t i = 0; i < gbc_app->files_count; i++)
                if (gbc_app->files[i].type == RETRO_TYPE_FILE)
                    total++;
        }

        if (total > 0)
        {
            entries = calloc(total, sizeof(display_entry_t));
            int idx = 0;

            if ((filter == DISPLAY_MODE_GB || filter == DISPLAY_MODE_SHUFFLE) && gb_app)
            {
                for (size_t i = 0; i < gb_app->files_count; i++)
                    if (gb_app->files[i].type == RETRO_TYPE_FILE)
                        entries[idx++] = (display_entry_t){gb_app, i};
            }
            if ((filter == DISPLAY_MODE_GBC || filter == DISPLAY_MODE_SHUFFLE) && gbc_app)
            {
                for (size_t i = 0; i < gbc_app->files_count; i++)
                    if (gbc_app->files[i].type == RETRO_TYPE_FILE)
                        entries[idx++] = (display_entry_t){gbc_app, i};
            }

            // Shuffle with stored seed for consistent order across reboots
            uint32_t seed = rg_settings_get_number(NS_GLOBAL, "DisplaySeed", 0);
            shuffle_entries(entries, total, seed);

            // Wrap index - all games played, new shuffle round
            if (index >= total)
            {
                seed = (uint32_t)rg_system_timer();
                shuffle_entries(entries, total, seed);
                rg_settings_set_number(NS_GLOBAL, "DisplaySeed", seed);
                index = 0;
            }

            // Save next index for the following cycle
            rg_settings_set_number(NS_GLOBAL, "DisplayIndex", index + 1);
            rg_settings_commit();

            // Launch the game at current index
            display_entry_t *entry = &entries[index];
            application_start_by_index(entry->app, entry->file_idx);
            // Won't reach here - system switches app
        }
        else
        {
            // No games found, deactivate display mode
            rg_settings_set_number(NS_GLOBAL, "DisplayActive", 0);
            rg_settings_commit();
        }

        free(entries);
    }

#ifdef RG_ENABLE_NETWORKING
    rg_network_init();
    if (rg_settings_get_number(NS_APP, SETTING_WEBUI, true))
        webui_start();
#endif

    if (!gui_get_current_tab())
        gui.selected_tab = 0;
    tab = gui_set_current_tab(gui.selected_tab);

    while (true)
    {
        // At the moment the HTTP server has absolute priority because it may change UI elements.
        // It's also risky to let the user do file accesses at the same time (thread safety, SPI, etc)...
        if (gui.http_lock)
        {
            rg_gui_draw_message(_("HTTP Server Busy..."));
            redraw_pending = true;
            rg_task_delay(100);
            continue;
        }

        prev_joystick = gui.joystick;
        joystick = 0;

        if ((gui.joystick = rg_input_read_gamepad()))
        {
            if (prev_joystick != gui.joystick)
            {
                joystick = gui.joystick;
                repeats = 0;
                next_repeat = rg_system_timer() + 400000;
            }
            else if ((rg_system_timer() - next_repeat) >= 0)
            {
                joystick = gui.joystick;
                repeats++;
                next_repeat = rg_system_timer() + 400000 / (repeats + 1);
            }
        }

        if (joystick & (RG_KEY_MENU|RG_KEY_OPTION))
        {
            if (joystick == RG_KEY_MENU)
                rg_gui_about_menu();
            else
                rg_gui_options_menu();

            gui_update_theme();
            gui_save_config();
            rg_settings_commit();
            redraw_pending = true;
        }

        int64_t start_time = rg_system_timer();

        if (!tab->enabled && !change_tab)
        {
            change_tab = 1;
        }

        if (change_tab || gui.browse != browse_last)
        {
            if (change_tab)
            {
                gui_event(TAB_LEAVE, tab);
                tab = gui_set_current_tab(gui.selected_tab + change_tab);
                for (int tabs = gui.tabs_count; !tab->enabled && --tabs > 0;)
                    tab = gui_set_current_tab(gui.selected_tab + change_tab);
                change_tab = 0;
            }

            if (gui.browse)
            {
                if (!tab->initialized)
                {
                    gui_redraw();
                    gui_init_tab(tab);
                }
                gui_event(TAB_ENTER, tab);
            }

            browse_last = gui.browse;
            redraw_pending = true;
        }

        if (gui.browse)
        {
            if (joystick == RG_KEY_SELECT) {
                change_tab = -1;
            }
            else if (joystick == RG_KEY_START) {
                change_tab = 1;
            }
            else if (joystick == RG_KEY_UP) {
                gui_scroll_list(tab, SCROLL_LINE, -1);
                redraw_pending = true;
            }
            else if (joystick == RG_KEY_DOWN) {
                gui_scroll_list(tab, SCROLL_LINE, 1);
                redraw_pending = true;
            }
            else if (joystick == RG_KEY_LEFT) {
                gui_scroll_list(tab, SCROLL_PAGE, -1);
                redraw_pending = true;
            }
            else if (joystick == RG_KEY_RIGHT) {
                gui_scroll_list(tab, SCROLL_PAGE, 1);
                redraw_pending = true;
            }
            else if (joystick == RG_KEY_A) {
                gui_event(TAB_ACTION, tab);
                redraw_pending = true;
            }
            else if (joystick == RG_KEY_B) {
                if (tab->navpath)
                    gui_event(TAB_BACK, tab);
                else
                    gui.browse = false;
                redraw_pending = true;
            }
        }
        else
        {
            if (joystick & (RG_KEY_UP|RG_KEY_LEFT|RG_KEY_SELECT)) {
                change_tab = -1;
            }
            else if (joystick & (RG_KEY_DOWN|RG_KEY_RIGHT|RG_KEY_START)) {
                change_tab = 1;
            }
            else if (joystick == RG_KEY_A) {
                gui.browse = true;
            }
        }

        if (redraw_pending)
        {
            redraw_pending = false;
            gui_redraw();
        }

        rg_system_tick(rg_system_timer() - start_time);

        if ((gui.joystick|joystick) & RG_KEY_ANY)
        {
            gui.idle_counter = 0;
            next_idle_event = rg_system_timer() + 100000;
        }
        else if (rg_system_timer() >= next_idle_event)
        {
            gui.idle_counter++;
            gui.joystick = 0;
            prev_joystick = 0;
            gui_event(TAB_IDLE, tab);
            next_idle_event = rg_system_timer() + 100000;
            if (gui.idle_counter % 10 == 1)
                redraw_pending = true;
        }
        else if (gui.idle_counter)
        {
            rg_task_delay(10);
        }
    }
}

static void try_migrate(void)
{
    // A handful of retro-go versions used the weird /odroid/*.txt to store books. Let's move them!
    if (rg_settings_get_number(NS_GLOBAL, "Migration", 0) < 1290)
    {
    #ifdef RG_TARGET_ODROID_GO
        rg_storage_mkdir(RG_BASE_PATH_CONFIG);
        rename(RG_STORAGE_ROOT "/odroid/favorite.txt", RG_BASE_PATH_CONFIG "/favorite.txt");
        rename(RG_STORAGE_ROOT "/odroid/recent.txt", RG_BASE_PATH_CONFIG "/recent.txt");
    #endif
        rg_settings_set_number(NS_GLOBAL, "Migration", 1290);
        rg_settings_commit();
    }

    // Some of our save formats have diverged and cause issue when they're shared with Go-Play
    if (rg_settings_get_number(NS_GLOBAL, "Migration", 0) < 1390)
    {
    #ifdef RG_TARGET_ODROID_GO
        if (rg_storage_exists(RG_STORAGE_ROOT "/odroid/data"))
            rg_gui_alert("Save path changed in 1.32",
                "Save format is no longer fully compatible with Go-Play and can cause corruption.\n\n"
                "Please copy the contents of:\n /odroid/data\nto\n /retro-go/saves.");
    #endif
        rg_settings_set_number(NS_GLOBAL, "Migration", 1390);
        rg_settings_commit();
    }

    if (rg_settings_get_number(NS_GLOBAL, "Migration", 0) < 1440)
    {
        // Bit order and default value of the indicators has changed in 1.44, reset it
        rg_settings_set_number(NS_GLOBAL, "Indicators", (1 << RG_INDICATOR_POWER_LOW));
        rg_settings_set_number(NS_GLOBAL, "Migration", 1440);
        rg_settings_commit();
    }
}

void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
        gui_redraw();
}

static void options_handler(rg_gui_option_t *dest)
{
    const rg_gui_option_t options[] = {
        {0, _("Color theme"),  "-", RG_DIALOG_FLAG_NORMAL, &color_theme_cb},
        #if RG_USB_MSC_SUPPORTED
        {0, _("USB Mode"),     NULL, RG_DIALOG_FLAG_NORMAL, &usb_mode_cb},
        #endif
        {0, _("Showcase mode"), "-", RG_DIALOG_FLAG_NORMAL, &display_mode_cb},
        {0, _("Preview"),      "-", RG_DIALOG_FLAG_NORMAL, &show_preview_cb},
        {0, _("Scroll mode"),  "-", RG_DIALOG_FLAG_NORMAL, &scroll_mode_cb},
        {0, _("Start screen"), "-", RG_DIALOG_FLAG_NORMAL, &start_screen_cb},
        {0, _("Boot animation"), "-", RG_DIALOG_FLAG_NORMAL, &bootanim_cb},
        {0, _("Hide tabs"),    "-", RG_DIALOG_FLAG_NORMAL, &toggle_tabs_cb},
        #ifdef RG_ENABLE_NETWORKING
        {0, _("File server"),  "-", RG_DIALOG_FLAG_NORMAL, &webui_switch_cb},
        #endif
        {0, _("Startup app"),  "-", RG_DIALOG_FLAG_NORMAL, &startup_app_cb},
        RG_DIALOG_END,
    };
    memcpy(dest, options, sizeof(options));
}

static void about_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Build CRC cache"), NULL, RG_DIALOG_FLAG_NORMAL, &prebuild_cache_cb};
    #if defined(RG_ENABLE_NETWORKING) && RG_UPDATER_ENABLE
    *dest++ = (rg_gui_option_t){0, _("Check for updates"), NULL, RG_DIALOG_FLAG_NORMAL, &updater_cb};
    #endif
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

void app_main(void)
{
    const rg_handlers_t handlers = {
        .event = &event_handler,
        .options = &options_handler,
        .about = &about_handler,
    };

    app = rg_system_init(32000, &handlers, NULL);
    app->configNs = "launcher";
    app->isLauncher = true;

    // Show boot animation (disabled - not working yet)
    // boot_animation();

    if (!rg_storage_ready())
    {
        rg_display_clear(C_SKY_BLUE);
        rg_gui_alert(_("SD Card Error"), _("Storage mount failed.\nMake sure the card is FAT32."));
    }
    else
    {
        rg_storage_mkdir(RG_BASE_PATH_CACHE);
        rg_storage_mkdir(RG_BASE_PATH_CONFIG);
        rg_storage_mkdir(RG_BASE_PATH_SAVES);
        rg_storage_mkdir(RG_BASE_PATH_BIOS);
        rg_storage_mkdir(RG_BASE_PATH_ROMS);
        rg_storage_mkdir(RG_BASE_PATH_ROMS "/nes");
        rg_storage_mkdir(RG_BASE_PATH_ROMS "/gb");
        rg_storage_mkdir(RG_BASE_PATH_ROMS "/gbc");
        rg_storage_mkdir(RG_BASE_PATH_ROMS "/gw");
        rg_storage_mkdir(RG_BASE_PATH_ROMS "/sms");
        rg_storage_mkdir(RG_BASE_PATH_ROMS "/gg");
        rg_storage_mkdir(RG_BASE_PATH_ROMS "/md");
        rg_storage_mkdir(RG_BASE_PATH_ROMS "/col");
        rg_storage_mkdir(RG_BASE_PATH_ROMS "/doom");
        try_migrate();
    }

#ifdef ESP_PLATFORM
    // The launcher makes a lot of small allocations and it sometimes fills internal RAM, causing the SD Card driver to
    // stop working. Lowering CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL and manually using rg_alloc to do internal allocs when
    // needed is a better solution, but that would have to be done for every app. This is a good workaround for now.
    heap_caps_malloc_extmem_enable(1024);
#endif

    retro_loop();
}
