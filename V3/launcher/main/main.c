#include <rg_system.h>
#include <sys/time.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#endif

#include "applications.h"
#include "bookmarks.h"
#include "browser.h"
#include "gui.h"
#include "webui.h"
#include "updater.h"
#include "usb_msc.h"
#include "magnet_launch.h"

static rg_app_t *app;

#ifdef RG_ENABLE_NETWORKING
// Background one-shot SNTP time sync for "Sync only" Wi-Fi mode. Runs silently
// (no on-screen message, no boot delay); the task self-deletes when it returns.
static void sync_time_task(void *arg)
{
    (void)arg;
    rg_system_sync_time_now();
}
#endif

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

    sprintf(option->value, "%d/%d", gui.color_theme + 1, max + 1);

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        return RG_DIALOG_REDRAW;

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

// ---- Showcase mode -------------------------------------------------------
// Showcase cycles through games on a timer. The mode + per-game switch time +
// optional sleep timer are configurable, and "Selected" mode rotates only the
// games the user ticked in the selection list (stored one path per line).

#define SHOWCASE_SEL_FILE        RG_BASE_PATH_CONFIG "/favorite.txt"
// Cached, pre-shuffled rotation (one "short_name<TAB>path" per line) so showcase
// game-to-game transitions launch the next game without re-scanning the SD card.
#define SHOWCASE_ROTATION_FILE   RG_BASE_PATH_CONFIG "/showcase_rotation.txt"
#define SHOWCASE_INTERVAL_DEFAULT 60   // seconds per game

// State shared with the selection-list toggle callbacks.
static bool        *sc_state;
static char       (*sc_values)[4];
static char       **sc_paths;
static const char **sc_names;
static int          sc_count;

// The selection list is shown in pages of this many games. A single rg_gui_dialog
// with hundreds of items is unusable (its first draw never completes / hangs), so
// we never put more than one page of rows in a dialog at once.
#define SC_PER_PAGE 50

// Reads the whole selection file into a malloc'd, NUL-terminated blob (or NULL).
static char *showcase_load_blob(void)
{
    FILE *fp = fopen(SHOWCASE_SEL_FILE, "r");
    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *blob = (size > 0) ? malloc(size + 1) : NULL;
    if (blob)
    {
        size_t got = fread(blob, 1, size, fp);
        blob[got] = 0;
    }
    fclose(fp);
    return blob;
}

// True if "folder/name" appears as its own line in the selection blob.
static bool showcase_path_listed(const char *blob, const char *folder, const char *name)
{
    if (!blob)
        return false;
    char path[RG_PATH_MAX + 1];
    snprintf(path, sizeof(path), "%s/%s", folder, name);
    size_t len = strlen(path);
    for (const char *p = blob; (p = strstr(p, path)) != NULL; p += len)
    {
        bool at_start = (p == blob) || (p[-1] == '\n') || (p[-1] == '\r');
        char after = p[len];
        if (at_start && (after == '\n' || after == '\r' || after == 0))
            return true;
    }
    return false;
}

// Builds the list of games for the given showcase mode. For DISPLAY_MODE_SELECTED
// only ticked games are included. Returns a malloc'd array (caller frees) and
// sets *out_count; returns NULL when there is nothing to show.
static display_entry_t *showcase_build_entries(int mode, int *out_count)
{
    *out_count = 0;
    retro_app_t *gb_app  = application_find("gb");
    retro_app_t *gbc_app = application_find("gbc");
    bool use_gb  = (mode == DISPLAY_MODE_GB  || mode == DISPLAY_MODE_SHUFFLE || mode == DISPLAY_MODE_SELECTED);
    bool use_gbc = (mode == DISPLAY_MODE_GBC || mode == DISPLAY_MODE_SHUFFLE || mode == DISPLAY_MODE_SELECTED);
    retro_app_t *apps[2] = { use_gb ? gb_app : NULL, use_gbc ? gbc_app : NULL };
    char *blob = (mode == DISPLAY_MODE_SELECTED) ? showcase_load_blob() : NULL;

    for (int a = 0; a < 2; a++)
        if (apps[a])
            application_start_by_index(apps[a], -1); // Init only (index -1 returns early)

    int total = 0;
    for (int a = 0; a < 2; a++)
    {
        retro_app_t *ap = apps[a];
        if (!ap) continue;
        for (size_t i = 0; i < ap->files_count; i++)
        {
            if (ap->files[i].type != RETRO_TYPE_FILE) continue;
            if (mode == DISPLAY_MODE_SELECTED && !showcase_path_listed(blob, ap->files[i].folder, ap->files[i].name)) continue;
            total++;
        }
    }

    display_entry_t *entries = (total > 0) ? calloc(total, sizeof(display_entry_t)) : NULL;
    if (entries)
    {
        int idx = 0;
        for (int a = 0; a < 2; a++)
        {
            retro_app_t *ap = apps[a];
            if (!ap) continue;
            for (size_t i = 0; i < ap->files_count; i++)
            {
                if (ap->files[i].type != RETRO_TYPE_FILE) continue;
                if (mode == DISPLAY_MODE_SELECTED && !showcase_path_listed(blob, ap->files[i].folder, ap->files[i].name)) continue;
                entries[idx++] = (display_entry_t){ap, (int)i};
            }
        }
        *out_count = total;
    }

    free(blob);
    return entries;
}

// Writes the (already shuffled) rotation to SHOWCASE_ROTATION_FILE, one
// "short_name<TAB>folder/name" per line, so later transitions can launch the next
// game without scanning the SD card again.
static void showcase_write_rotation(display_entry_t *entries, int total)
{
    FILE *fp = fopen(SHOWCASE_ROTATION_FILE, "w");
    if (!fp && rg_storage_mkdir(RG_BASE_PATH_CONFIG))
        fp = fopen(SHOWCASE_ROTATION_FILE, "w");
    if (!fp)
        return;
    for (int i = 0; i < total; i++)
    {
        retro_app_t *app = entries[i].app;
        retro_file_t *f = &app->files[entries[i].file_idx];
        fprintf(fp, "%s\t%s/%s\n", app->short_name, f->folder, f->name);
    }
    fclose(fp);
}

// Launches the game at `index` in the cached rotation WITHOUT scanning the SD
// card. Returns false (without launching) when the cache is missing/empty/invalid
// so the caller can fall back to building the rotation from a scan.
static bool showcase_launch_from_cache(int index)
{
    FILE *fp = fopen(SHOWCASE_ROTATION_FILE, "r");
    if (!fp)
        return false;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *blob = (size > 0) ? rg_alloc(size + 1, MEM_SLOW) : NULL;
    if (blob)
    {
        size_t got = fread(blob, 1, size, fp);
        blob[got] = 0;
    }
    fclose(fp);
    if (!blob)
        return false;

    // Split into lines (each "short_name\tfolder/name").
    int count = 0;
    for (char *p = blob; *p; ++p)
        if (*p == '\n') count++;
    char **lines = (count > 0) ? rg_alloc(count * sizeof(char *), MEM_SLOW) : NULL;
    if (!lines)
    {
        free(blob);
        return false;
    }
    int n = 0;
    for (char *p = blob; *p && n < count; )
    {
        lines[n++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        p = nl + 1;
    }
    count = n;
    if (count == 0)
    {
        free(lines);
        free(blob);
        return false;
    }

    // Played the whole rotation: reshuffle the lines, persist them, restart at 0.
    if (index >= count)
    {
        uint32_t seed = (uint32_t)rg_system_timer();
        for (int i = count - 1; i > 0; i--)
        {
            seed = seed * 1103515245 + 12345;
            int j = (seed >> 16) % (i + 1);
            char *t = lines[i]; lines[i] = lines[j]; lines[j] = t;
        }
        index = 0;
        FILE *out = fopen(SHOWCASE_ROTATION_FILE, "w");
        if (out)
        {
            for (int i = 0; i < count; i++)
                fprintf(out, "%s\n", lines[i]);
            fclose(out);
        }
    }

    char *line = lines[index];
    char *tab = strchr(line, '\t');
    if (!tab)
    {
        free(lines);
        free(blob);
        return false;
    }
    *tab = 0;
    const char *short_name = line;
    char *path = tab + 1;
    retro_app_t *app = application_find(short_name);
    if (app)
    {
        rg_settings_set_number(NS_GLOBAL, "DisplayIndex", index + 1);
        rg_settings_commit();
        rg_system_switch_app(app->partition, app->short_name, path, RG_BOOT_ONCE); // no return
    }

    free(lines);
    free(blob);
    return false;
}

static rg_gui_event_t showcase_mode_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    const char *modes[DISPLAY_MODE_COUNT] = {_("Off"), "GB", "GBC", _("Shuffle"), _("Favorites")};

    if (event == RG_DIALOG_ENTER)
    {
        // Popup list: pick the mode directly (current one pre-selected).
        rg_gui_option_t opts[DISPLAY_MODE_COUNT + 1];
        for (int i = 0; i < DISPLAY_MODE_COUNT; i++)
            opts[i] = (rg_gui_option_t){i, modes[i], NULL, RG_DIALOG_FLAG_NORMAL, NULL};
        opts[DISPLAY_MODE_COUNT] = (rg_gui_option_t)RG_DIALOG_END;
        int sel = rg_gui_dialog(_("Mode"), opts, gui.display_mode % DISPLAY_MODE_COUNT);
        if (sel >= 0 && sel < DISPLAY_MODE_COUNT)
            gui.display_mode = sel;
    }

    strcpy(option->value, modes[gui.display_mode % DISPLAY_MODE_COUNT]);
    return (event == RG_DIALOG_ENTER) ? RG_DIALOG_REDRAW : RG_DIALOG_VOID;
}

// "Switch time" is a popup picker with separate Minutes (0-10) and Seconds
// (0-55, step 5) fields, like "Sleep after". Stored as total seconds in
// DisplayInterval, clamped to 10s..10min.
static int interval_clamp(int v) { return v < 10 ? 10 : (v > 600 ? 600 : v); }

static rg_gui_event_t interval_field_cb(rg_gui_option_t *option, rg_gui_event_t event, bool minutes)
{
    int v = interval_clamp(rg_settings_get_number(NS_GLOBAL, "DisplayInterval", SHOWCASE_INTERVAL_DEFAULT));
    int m = v / 60, s = v % 60;
    if (minutes)
    {
        if (event == RG_DIALOG_PREV) m = (m > 0) ? m - 1 : 10;
        if (event == RG_DIALOG_NEXT) m = (m < 10) ? m + 1 : 0;
    }
    else
    {
        if (event == RG_DIALOG_PREV) s = (s >= 5) ? s - 5 : 55;
        if (event == RG_DIALOG_NEXT) s = (s <= 50) ? s + 5 : 0;
    }
    v = interval_clamp(m * 60 + s);
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_settings_set_number(NS_GLOBAL, "DisplayInterval", v);
    sprintf(option->value, "%02d", (minutes ? m : s) % 100);
    return RG_DIALOG_VOID;
}
static rg_gui_event_t interval_m_cb(rg_gui_option_t *o, rg_gui_event_t e) { return interval_field_cb(o, e, true);  }
static rg_gui_event_t interval_s_cb(rg_gui_option_t *o, rg_gui_event_t e) { return interval_field_cb(o, e, false); }

static void interval_summary(char *out)
{
    int v = interval_clamp(rg_settings_get_number(NS_GLOBAL, "DisplayInterval", SHOWCASE_INTERVAL_DEFAULT));
    int m = (v / 60) % 100, s = v % 60;
    if (v < 60)      sprintf(out, "%ds", s);
    else if (s == 0) sprintf(out, "%dm", m);
    else             sprintf(out, "%dm %ds", m, s);
}

static rg_gui_event_t showcase_interval_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        char m_v[16], s_v[16];
        interval_m_cb(&(rg_gui_option_t){0, NULL, m_v, 0, NULL}, RG_DIALOG_VOID);
        interval_s_cb(&(rg_gui_option_t){0, NULL, s_v, 0, NULL}, RG_DIALOG_VOID);
        const rg_gui_option_t opts[] = {
            {0, _("Minutes"), m_v, RG_DIALOG_FLAG_NORMAL, &interval_m_cb},
            {0, _("Seconds"), s_v, RG_DIALOG_FLAG_NORMAL, &interval_s_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(_("Switch time"), opts, 0);
        rg_settings_commit();
    }
    interval_summary(option->value);
    return (event == RG_DIALOG_ENTER) ? RG_DIALOG_REDRAW : RG_DIALOG_VOID;
}

static rg_gui_event_t sc_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int i = (int)option->arg;
    if (event == RG_DIALOG_ENTER || event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        sc_state[i] = !sc_state[i];
    // Write to option->value: rg_gui_dialog repoints it at its own text buffer,
    // and re-invokes this callback with RG_DIALOG_UPDATE on every redraw.
    strcpy(option->value, sc_state[i] ? "[x]" : "[ ]");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t sc_all_cb(rg_gui_option_t *option, rg_gui_event_t event, bool on)
{
    if (event == RG_DIALOG_ENTER)
    {
        // Just flip the state; the REDRAW re-runs sc_toggle_cb for every row,
        // which refreshes each row's displayed "[x]"/"[ ]".
        for (int i = 0; i < sc_count; i++)
            sc_state[i] = on;
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}
static rg_gui_event_t sc_all_on_cb(rg_gui_option_t *o, rg_gui_event_t e)  { return sc_all_cb(o, e, true); }
static rg_gui_event_t sc_all_off_cb(rg_gui_option_t *o, rg_gui_event_t e) { return sc_all_cb(o, e, false); }

static rg_gui_event_t showcase_select_games_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    // First entry per session scans the gb/gbc ROM folders, which can take a
    // while with many ROMs on a slow SD card. Show a message so it doesn't look
    // frozen (scan_folder_cb draws a running count while it works).
    rg_gui_draw_message(_("Scanning games..."));
    application_set_scan_progress(true); // show the per-file counter only here
    retro_app_t *apps[2] = { application_find("gb"), application_find("gbc") };
    for (int a = 0; a < 2; a++)
        if (apps[a])
            application_start_by_index(apps[a], -1);
    application_set_scan_progress(false);

    int total = 0;
    for (int a = 0; a < 2; a++)
        if (apps[a])
            for (size_t i = 0; i < apps[a]->files_count; i++)
                if (apps[a]->files[i].type == RETRO_TYPE_FILE)
                    total++;

    if (total == 0)
    {
        rg_gui_alert(_("Showcase mode"), _("No games found!"));
        return RG_DIALOG_VOID;
    }

    sc_count  = total;
    // All per-game arrays live in PSRAM (MEM_SLOW) so they don't exhaust the small
    // pool of internal RAM with many ROMs (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=32KB
    // forces sub-32KB allocations into internal RAM). rg_alloc zeroes and frees with free().
    sc_state  = rg_alloc(total * sizeof(bool), MEM_SLOW);
    sc_values = rg_alloc(total * sizeof(*sc_values), MEM_SLOW);
    sc_paths  = rg_alloc(total * sizeof(char *), MEM_SLOW);
    sc_names  = rg_alloc(total * sizeof(char *), MEM_SLOW);
    // opts only ever holds one page: All on/off + optional Prev/Next + SC_PER_PAGE rows + END.
    rg_gui_option_t *opts = rg_alloc((SC_PER_PAGE + 5) * sizeof(rg_gui_option_t), MEM_SLOW);
    char *blob = NULL;
    int idx = 0;
    if (!sc_state || !sc_values || !sc_paths || !sc_names || !opts)
        goto cleanup;

    blob = showcase_load_blob();

    // Build the per-game arrays once: path (for saving), name (label) and ticked state.
    for (int a = 0; a < 2; a++)
    {
        retro_app_t *ap = apps[a];
        if (!ap) continue;
        for (size_t i = 0; i < ap->files_count; i++)
        {
            if (ap->files[i].type != RETRO_TYPE_FILE) continue;
            char path[RG_PATH_MAX + 1];
            snprintf(path, sizeof(path), "%s/%s", ap->files[i].folder, ap->files[i].name);
            size_t plen = strlen(path) + 1;
            sc_paths[idx] = rg_alloc(plen, MEM_SLOW); // PSRAM, not internal strdup
            if (sc_paths[idx]) memcpy(sc_paths[idx], path, plen);
            sc_names[idx] = ap->files[i].name;
            sc_state[idx] = showcase_path_listed(blob, ap->files[i].folder, ap->files[i].name);
            idx++;
            if ((idx & 63) == 0)
                rg_gui_draw_message(_("Preparing list... %d/%d"), idx, total);
        }
    }
    free(blob);
    blob = NULL;

    // Show the list one page at a time. Prev/Next are plain items (no callback) so
    // pressing A on them returns their arg (RG_DIALOG_SELECT); games toggle in place.
    {
        int page = 0;
        int pages = (total + SC_PER_PAGE - 1) / SC_PER_PAGE;
        while (1)
        {
            int start = page * SC_PER_PAGE;
            int end = start + SC_PER_PAGE;
            if (end > total) end = total;
            int n = 0;
            opts[n++] = (rg_gui_option_t){0, "* All on",  NULL, RG_DIALOG_FLAG_NORMAL, &sc_all_on_cb};
            opts[n++] = (rg_gui_option_t){0, "* All off", NULL, RG_DIALOG_FLAG_NORMAL, &sc_all_off_cb};
            // Nav at the top so it's always visible without scrolling past 50 games.
            if (page > 0)
                opts[n++] = (rg_gui_option_t){-2, "< Previous page", NULL, RG_DIALOG_FLAG_NORMAL, NULL};
            if (end < total)
                opts[n++] = (rg_gui_option_t){-3, "Next page >", NULL, RG_DIALOG_FLAG_NORMAL, NULL};
            for (int i = start; i < end; i++)
            {
                strcpy(sc_values[i], sc_state[i] ? "[x]" : "[ ]");
                opts[n++] = (rg_gui_option_t){(intptr_t)i, sc_names[i], sc_values[i], RG_DIALOG_FLAG_NORMAL, &sc_toggle_cb};
            }
            opts[n] = (rg_gui_option_t)RG_DIALOG_END;

            char title[48];
            snprintf(title, sizeof(title), "%s (%d/%d)", _("Edit favorites"), page + 1, pages);
            intptr_t r = rg_gui_dialog(title, opts, 0);
            if (r == -2) { page--; continue; }
            if (r == -3) { page++; continue; }
            break; // B / cancel: done
        }
    }

    // Save the ticked games (one path per line).
    FILE *fp = fopen(SHOWCASE_SEL_FILE, "w");
    if (!fp && rg_storage_mkdir(RG_BASE_PATH_CONFIG))
        fp = fopen(SHOWCASE_SEL_FILE, "w");
    if (fp)
    {
        for (int i = 0; i < total; i++)
            if (sc_state[i] && sc_paths[i])
                fprintf(fp, "%s\n", sc_paths[i]);
        fclose(fp);
    }

    // The ticked set changed, so any cached showcase rotation is now stale.
    remove(SHOWCASE_ROTATION_FILE);

    // If at least one game is ticked, switch showcase to "Selected" so that
    // Start showcase rotates only those. Otherwise the Mode would still be
    // GB/GBC/Shuffle and showcase would play everything.
    {
        int selected = 0;
        for (int i = 0; i < total; i++)
            if (sc_state[i]) selected++;
        if (selected > 0 && gui.display_mode != DISPLAY_MODE_SELECTED)
        {
            gui.display_mode = DISPLAY_MODE_SELECTED;
            gui_save_config();
            rg_settings_commit();
        }
    }

cleanup:
    free(blob);
    if (sc_paths)
        for (int i = 0; i < total; i++)
            free(sc_paths[i]);
    free(sc_paths);
    free(sc_state);
    free(sc_values);
    free(sc_names);
    free(opts);
    sc_paths = NULL; sc_state = NULL; sc_values = NULL; sc_names = NULL; sc_count = 0;
    return RG_DIALOG_REDRAW;
}

// Starts showcase right now: builds the rotation for the current display_mode,
// marks it active and launches the first game. On success it does NOT return
// (the system switches to the game). Returns false (without launching) when the
// mode yields no games, so callers can show an alert.
static bool showcase_begin(void)
{
    int total = 0;
    display_entry_t *entries = showcase_build_entries(gui.display_mode, &total);
    if (!entries || total == 0)
    {
        free(entries);
        return false;
    }

    uint32_t seed = (uint32_t)rg_system_timer();
    shuffle_entries(entries, total, seed);
    showcase_write_rotation(entries, total); // cache so transitions skip the scan

    rg_settings_set_number(NS_GLOBAL, "DisplayActive", 1);
    rg_settings_set_number(NS_GLOBAL, "DisplayFilter", gui.display_mode);
    rg_settings_set_number(NS_GLOBAL, "DisplayIndex", 1);
    rg_settings_set_number(NS_GLOBAL, "DisplaySeed", seed);
    rg_settings_set_number(NS_GLOBAL, "DisplayElapsedSec", 0); // reset sleep accounting
    gui_save_config();
    rg_settings_commit();

    application_start_by_index(entries[0].app, entries[0].file_idx);
    free(entries); // Won't reach here
    return true;
}

static rg_gui_event_t showcase_start_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    if (gui.display_mode == DISPLAY_MODE_OFF)
    {
        rg_gui_alert(_("Showcase mode"), _("Set a mode first!"));
        return RG_DIALOG_VOID;
    }

    if (!showcase_begin())
        rg_gui_alert(_("Showcase mode"), _("No games found!"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t schedule_menu_cb(rg_gui_option_t *option, rg_gui_event_t event); // defined below

static rg_gui_event_t display_mode_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    char mode_v[16], int_v[16];
    showcase_mode_cb(&(rg_gui_option_t){0, NULL, mode_v, 0, NULL}, RG_DIALOG_VOID);
    showcase_interval_cb(&(rg_gui_option_t){0, NULL, int_v, 0, NULL}, RG_DIALOG_VOID);

    const rg_gui_option_t opts[] = {
        {0, _("Start showcase"), NULL,    RG_DIALOG_FLAG_NORMAL, &showcase_start_cb},
        {0, _("Mode"),           mode_v,  RG_DIALOG_FLAG_NORMAL, &showcase_mode_cb},
        {0, _("Switch time"),    int_v,   RG_DIALOG_FLAG_NORMAL, &showcase_interval_cb},
        {0, _("Edit favorites"),   NULL,    RG_DIALOG_FLAG_NORMAL, &showcase_select_games_cb},
        {0, _("Sleep timer"),    NULL,    RG_DIALOG_FLAG_NORMAL, &schedule_menu_cb},
        RG_DIALOG_END,
    };
    rg_gui_dialog(_("Showcase mode"), opts, 0);
    gui_save_config();
    rg_settings_commit();
    return RG_DIALOG_VOID;
}

// ---- Sleep timer ----------------------------------------------------------
// A daily sleep window: the device sleeps from "Sleep from" until "Wake up"
// and is awake (showcasing) the rest of the day. Implemented with a deep-sleep
// timer wakeup (logic lives in rg_system.c). It only works when the clock is
// correct, and there is no battery-backed RTC, so the clock must be re-set
// after a full power loss (Settings > Clock). The menu shows the current clock
// so a wrong time is easy to spot. Internally "Wake up" is stored as SchedOnMin
// and "Sleep from" as SchedOffMin; the awake window is [On, Off).
static int clock_is_24h(void) { return rg_settings_get_number(NS_GLOBAL, "Clock24h", 1); }

// Format an hour (0..23) honoring the 12/24-hour setting.
static void fmt_hour(char *out, int h)
{
    if (clock_is_24h())
        sprintf(out, "%02d", h % 100);
    else
    {
        int h12 = h % 12; if (h12 == 0) h12 = 12;
        sprintf(out, "%d%s", h12, (h % 24 < 12) ? "AM" : "PM");
    }
}

// Format HH:MM honoring the 12/24-hour setting.
static void fmt_time(char *out, int h, int m)
{
    if (clock_is_24h())
        sprintf(out, "%02d:%02d", h % 100, m % 60);
    else
    {
        int h12 = h % 12; if (h12 == 0) h12 = 12;
        sprintf(out, "%d:%02d%s", h12, m % 60, (h % 24 < 12) ? "AM" : "PM");
    }
}

static int sched_clamp(int v) { return v < 0 ? 0 : (v > 1439 ? 1439 : v); }

static rg_gui_event_t sched_field_cb(rg_gui_option_t *option, rg_gui_event_t event, const char *key, bool hours)
{
    int v = sched_clamp(rg_settings_get_number(NS_GLOBAL, key, 0));
    int h = v / 60, m = v % 60;
    if (hours)
    {
        if (event == RG_DIALOG_PREV) h = (h > 0) ? h - 1 : 23;
        if (event == RG_DIALOG_NEXT) h = (h < 23) ? h + 1 : 0;
    }
    else
    {
        if (event == RG_DIALOG_PREV) m = (m > 0) ? m - 1 : 59;
        if (event == RG_DIALOG_NEXT) m = (m < 59) ? m + 1 : 0;
    }
    v = h * 60 + m;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_settings_set_number(NS_GLOBAL, key, v);
    if (hours) fmt_hour(option->value, h);
    else       sprintf(option->value, "%02d", m % 100);
    return RG_DIALOG_VOID;
}
static rg_gui_event_t sched_on_h_cb(rg_gui_option_t *o, rg_gui_event_t e)  { return sched_field_cb(o, e, "SchedOnMin",  true);  }
static rg_gui_event_t sched_on_m_cb(rg_gui_option_t *o, rg_gui_event_t e)  { return sched_field_cb(o, e, "SchedOnMin",  false); }
static rg_gui_event_t sched_off_h_cb(rg_gui_option_t *o, rg_gui_event_t e) { return sched_field_cb(o, e, "SchedOffMin", true);  }
static rg_gui_event_t sched_off_m_cb(rg_gui_option_t *o, rg_gui_event_t e) { return sched_field_cb(o, e, "SchedOffMin", false); }

static void sched_time_picker(const char *title, rg_gui_callback_t h_cb, rg_gui_callback_t m_cb)
{
    char h_v[16], m_v[16];
    h_cb(&(rg_gui_option_t){0, NULL, h_v, 0, NULL}, RG_DIALOG_VOID);
    m_cb(&(rg_gui_option_t){0, NULL, m_v, 0, NULL}, RG_DIALOG_VOID);
    const rg_gui_option_t opts[] = {
        {0, _("Hour"),   h_v, RG_DIALOG_FLAG_NORMAL, h_cb},
        {0, _("Minute"), m_v, RG_DIALOG_FLAG_NORMAL, m_cb},
        RG_DIALOG_END,
    };
    rg_gui_dialog(title, opts, 0);
}

static void sched_summary(const char *key, char *out)
{
    int v = sched_clamp(rg_settings_get_number(NS_GLOBAL, key, 0));
    fmt_time(out, v / 60, v % 60);
}

static rg_gui_event_t sched_enable_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int en = rg_settings_get_number(NS_GLOBAL, "SchedEnable", 0);
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
    {
        en = !en;
        rg_settings_set_number(NS_GLOBAL, "SchedEnable", en);
        if (en)
        {
            // Turning the schedule On starts showcase straight away: the device
            // begins demoing and the per-second tick then sleeps/wakes it around
            // the [On, Off) window (sleeps now if we're outside it). Default to
            // Shuffle when no mode was picked, matching the timer-wake behaviour.
            if (gui.display_mode == DISPLAY_MODE_OFF)
                gui.display_mode = DISPLAY_MODE_SHUFFLE;
            rg_settings_commit();
            if (!showcase_begin()) // launches a game; does not return on success
            {
                rg_settings_set_number(NS_GLOBAL, "SchedEnable", 0);
                en = 0;
                rg_gui_alert(_("Sleep timer"), _("No games found!"));
            }
        }
        else
        {
            // Turning it Off also stops any running showcase.
            rg_settings_set_number(NS_GLOBAL, "DisplayActive", 0);
            rg_settings_commit();
        }
    }
    strcpy(option->value, en ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

// "Wake up" time = SchedOnMin (when the device comes back on).
static rg_gui_event_t sched_on_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER) { sched_time_picker(_("Wake up"), &sched_on_h_cb, &sched_on_m_cb); rg_settings_commit(); }
    sched_summary("SchedOnMin", option->value);
    return (event == RG_DIALOG_ENTER) ? RG_DIALOG_REDRAW : RG_DIALOG_VOID;
}
// "Sleep from" time = SchedOffMin (when the device goes to sleep).
static rg_gui_event_t sched_off_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER) { sched_time_picker(_("Sleep from"), &sched_off_h_cb, &sched_off_m_cb); rg_settings_commit(); }
    sched_summary("SchedOffMin", option->value);
    return (event == RG_DIALOG_ENTER) ? RG_DIALOG_REDRAW : RG_DIALOG_VOID;
}

static rg_gui_event_t schedule_menu_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    // Read-only line showing what the device currently thinks the time is, so a
    // wrong/unset clock (the usual reason the schedule "does nothing") is obvious.
    char clock_v[16];
    time_t now = time(NULL);
    struct tm lt = *localtime(&now);
    fmt_time(clock_v, lt.tm_hour, lt.tm_min);

    char en_v[16], on_v[16], off_v[16];
    sched_enable_cb(&(rg_gui_option_t){0, NULL, en_v, 0, NULL}, RG_DIALOG_VOID);
    sched_summary("SchedOnMin", on_v);
    sched_summary("SchedOffMin", off_v);
    const rg_gui_option_t opts[] = {
        {0, _("Clock now"),  clock_v, RG_DIALOG_FLAG_SKIP,   NULL},
        {0, _("Start"),      en_v,    RG_DIALOG_FLAG_NORMAL, &sched_enable_cb},
        {0, _("Sleep from"), off_v,   RG_DIALOG_FLAG_NORMAL, &sched_off_cb},
        {0, _("Wake up"),    on_v,    RG_DIALOG_FLAG_NORMAL, &sched_on_cb},
        RG_DIALOG_END,
    };
    rg_gui_dialog(_("Sleep timer"), opts, 1); // start on Start (row 0 is the read-only clock)
    rg_settings_commit();
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

#if defined(RG_ENABLE_NETWORKING) && RG_UPDATER_ENABLE
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

// Kept in sync with webui.c -- the menu shows and edits the same setting.
#define SETTING_WEBUI_PASS "WebUIPassword"



// Overrides the weak hooks in rg_gui.c: opening the "SD card in browser" info
// dialog starts the file server for this session (not persisted -- the "File
// server" toggle is what makes it survive a reboot) and shows the password it
// will ask for.
bool rg_gui_file_server_start(void)
{
    return webui_start(); // no-op if already running
}

// --- Hooks for the Wi-Fi menu -----------------------------------------------
// The file server and its password live with the rest of the server details
// under Wi-Fi rather than off in the system tab. That menu is built in the
// shared component, which cannot reach in here, so it asks via these hooks.

bool rg_gui_file_server_set_password(void)
{
    char *current = rg_settings_get_string(NS_APP, SETTING_WEBUI_PASS, webui_default_password());
    char *entered = rg_gui_input_str(_("Server password"), _("Password:"), current ? current : "");
    bool changed = false;
    if (entered)
    {
        rg_settings_set_string(NS_APP, SETTING_WEBUI_PASS, entered);
        rg_settings_commit();
        webui_reload_password(); // applies to the next request, no restart
        free(entered);
        changed = true;
    }
    free(current);
    return changed;
}

bool rg_gui_file_server_set_enabled(bool enable)
{
    webui_stop();
    if (enable && !webui_start())
        return false; // no Wi-Fi, or no memory for the transfer buffer
    rg_settings_set_number(NS_APP, SETTING_WEBUI, enable);
    rg_settings_commit();
    return true;
}

bool rg_gui_file_server_running(void)
{
    return webui_running();
}

const char *rg_gui_file_server_password(void)
{
    static char pass[33];
    char *stored = rg_settings_get_string(NS_APP, SETTING_WEBUI_PASS, webui_default_password());
    snprintf(pass, sizeof(pass), "%s", stored ? stored : "");
    free(stored);
    return pass[0] ? pass : NULL;
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

    // Auto on/off schedule is a showcase feature. On a cold boot with the
    // schedule enabled:
    //   - woke from the schedule timer (daily On time) -> (re)start showcase so
    //     the device demos itself; the auto-launch block below launches game 0.
    //   - any other wake (button / power-on) -> the user woke it manually, so
    //     stop showcase AND disarm the schedule (SchedEnable=0). Otherwise the
    //     schedule would linger and could sleep the device mid-game later on.
    //     They re-activate it via Auto on/off (or Start showcase).
    // Software resets (showcase's own game-switches) are not cold boots, so this
    // never disturbs a running rotation.
#ifdef ESP_PLATFORM
    if (app->isColdBoot && rg_settings_get_number(NS_GLOBAL, "SchedEnable", 0))
    {
        if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER)
        {
            int mode = gui.display_mode;
            if (mode == DISPLAY_MODE_OFF)
                mode = DISPLAY_MODE_SHUFFLE;
            rg_settings_set_number(NS_GLOBAL, "DisplayActive", 1);
            rg_settings_set_number(NS_GLOBAL, "DisplayFilter", mode);
            rg_settings_set_number(NS_GLOBAL, "DisplayIndex", 0);
            rg_settings_set_number(NS_GLOBAL, "DisplaySeed", (uint32_t)rg_system_timer());
            rg_settings_set_number(NS_GLOBAL, "DisplayElapsedSec", 0);
        }
        else
        {
            // Manual wake / power-on: stop showcase and disarm the schedule so
            // it can't sleep the device while the user is playing. They turn it
            // back on via Auto on/off.
            rg_settings_set_number(NS_GLOBAL, "DisplayActive", 0);
            rg_settings_set_number(NS_GLOBAL, "DisplayElapsedSec", 0);
            rg_settings_set_number(NS_GLOBAL, "SchedEnable", 0);
        }
        rg_settings_commit();
    }
#endif

    // USB mode armed before a reboot: we rebooted on purpose so this boot has no
    // UAC host loaded (audio is on Ext DAC), leaving the USB-OTG controller free for
    // device mode. Enter USB mode now; usb_msc_run_with_ui() restores USB-C DAC and
    // reboots back when the user disconnects, so it does not return here.
    if (rg_settings_get_number(NS_GLOBAL, "UsbModeBoot", 0))
        usb_msc_run_with_ui();

    // Magnet-cartridge auto-launch: reads the two Hall sensors, and if a
    // cartridge is detected and mapped to a ROM, switches to the emulator.
    // No-ops when disabled or no cartridge inserted.
    // Only on cold boot (power-on). When the launcher is re-entered via a
    // software reset (quitting a game), skip the auto-launch so the user
    // actually reaches the launcher instead of being bounced straight back
    // into the inserted cartridge's game.
    // Deliberately placed here, after the schedule and USB-mode blocks: a
    // showcase cartridge sets DisplayActive, which the schedule block clears on
    // a manual power-on, and an explicit USB-mode request should win over a
    // cartridge. It must still run before the DisplayActive auto-launch below,
    // which is what actually starts the showcase rotation.
    if (app->isColdBoot)
        magnet_launch_check_and_boot();

    // Display mode auto-launch: if active, launch the next game immediately
    if (rg_settings_get_number(NS_GLOBAL, "DisplayActive", 0))
    {
        int filter = rg_settings_get_number(NS_GLOBAL, "DisplayFilter", DISPLAY_MODE_GB);
        int index = rg_settings_get_number(NS_GLOBAL, "DisplayIndex", 0);

        // Fast path: launch the next game straight from the cached rotation (no SD
        // scan). Does not return on success; only falls through if the cache is
        // missing/invalid, in which case we rebuild it from a scan below.
        showcase_launch_from_cache(index);

        // Build the game list for this mode (Selected mode filters to ticked games)
        int total = 0;
        display_entry_t *entries = showcase_build_entries(filter, &total);

        if (total > 0)
        {
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

            // Cache this rotation so the next transitions skip the scan entirely.
            showcase_write_rotation(entries, total);

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
    // "Sync only" Wi-Fi mode: fetch the time in the BACKGROUND (no on-screen
    // message, no boot delay). The clock jumps to the right time a few seconds
    // after the launcher appears, then Wi-Fi turns off again. webui only makes
    // sense when Wi-Fi stays on, so it's limited to "On" mode.
    if (rg_system_get_wifi_mode() == RG_WIFI_MODE_SYNC_ONLY)
        rg_task_create("time_sync", &sync_time_task, NULL, 4 * 1024, RG_TASK_PRIORITY_2, -1);
    else if (rg_system_get_wifi_mode() == RG_WIFI_MODE_ON
             // Default OFF: the webui has no authentication and gives anyone
             // on the same network read/write access to the whole SD card
             // (incl. stored WiFi passwords in the settings). Whoever needs it
             // enables it deliberately through the menu.
             && rg_settings_get_number(NS_APP, SETTING_WEBUI, true))
        webui_start();
#endif

    if (!gui_get_current_tab())
        gui.selected_tab = 0;
    tab = gui_set_current_tab(gui.selected_tab);

    while (true)
    {
        // A ROM launch requested from the web UI is performed HERE, in the main
        // task, so the app switch (which does heavy display/SPI work) never races
        // the GUI drawing from a background HTTP task.
        if (gui.pending_launch)
        {
            char *path = gui.pending_launch;
            gui.pending_launch = NULL;
            application_start_path(path); // reboots into the emulator on success
            free(path);
            continue;
        }

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
                "Please copy the contents of:\n /odroid/data\nto\n /BlockBoy/saves.");
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
        {0, _("Startup app"),  "-", RG_DIALOG_FLAG_NORMAL, &startup_app_cb},
        {0, _("Magnet mode"),  NULL, RG_DIALOG_FLAG_NORMAL, &magnet_launch_options_cb},
        #ifdef RG_ENABLE_NETWORKING
        #if RG_UPDATER_ENABLE
        {0, _("Firmware update"), NULL, RG_DIALOG_FLAG_NORMAL, &updater_cb},
        #endif
        #endif
        RG_DIALOG_END,
    };
    memcpy(dest, options, sizeof(options));
}

static void about_handler(rg_gui_option_t *dest)
{
    if (rg_gui_get_option_visible("BuildCRC", false))
        *dest++ = (rg_gui_option_t){0, _("Build CRC cache"), NULL, RG_DIALOG_FLAG_NORMAL, &prebuild_cache_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

static void tabbed_options_handler(rg_gui_menu_entry_t *dest)
{
    const rg_gui_menu_entry_t entries[] = {
        // -------- General (tab 0) -- launcher UI preferences --------
        { 0, "ColorTheme",   false, {0, _("Launcher colors"), "-",  RG_DIALOG_FLAG_NORMAL, &color_theme_cb} },
        { 0, "StartScreen",  false, {0, _("Start screen"),    "-",  RG_DIALOG_FLAG_NORMAL, &start_screen_cb} },
        { 0, "Preview",      false, {0, _("Preview"),         "-",  RG_DIALOG_FLAG_NORMAL, &show_preview_cb} },
        { 0, "ScrollMode",   false, {0, _("Scroll mode"),     "-",  RG_DIALOG_FLAG_NORMAL, &scroll_mode_cb} },
        { 0, "HideTabs",     false, {0, _("Hide tabs"),       "-",  RG_DIALOG_FLAG_NORMAL, &toggle_tabs_cb} },

        // -------- System (tab 1) -- most-used first, then setup, then boot config --------
        { 1, "ShowcaseMode", true,  {0, _("Showcase mode"),   "-",  RG_DIALOG_FLAG_NORMAL, &display_mode_cb} },
        #if RG_USB_MSC_SUPPORTED
        { 1, "USBMode",      true,  {0, _("USB Mode"),        NULL, RG_DIALOG_FLAG_NORMAL, &usb_mode_cb} },
        #endif
        { 1, "MagnetMode",   true,  {0, _("Magnet cartridges"), NULL, RG_DIALOG_FLAG_NORMAL, &magnet_launch_options_cb} },
        // Hidden by default; the shop menu can only toggle entries in this list,
        // so the diagnostics live here rather than inside the menu above.
        { 1, "MagnetAdv",    false, {0, _("Magnet advanced"), NULL, RG_DIALOG_FLAG_NORMAL, &magnet_launch_advanced_cb} },
        { 1, "BootAnim",     true,  {0, _("Boot animation"),  "-",  RG_DIALOG_FLAG_NORMAL, &bootanim_cb} },
        { 1, "StartupApp",   true,  {0, _("Startup app"),     "-",  RG_DIALOG_FLAG_NORMAL, &startup_app_cb} },
        #ifdef RG_ENABLE_NETWORKING
        #if RG_UPDATER_ENABLE
        { 1, "Updater",      true,  {0, _("Firmware update"), NULL, RG_DIALOG_FLAG_NORMAL, &updater_cb} },
        #endif
        #endif
        { 1, "BuildCRC",     false, {0, _("Build CRC cache"), NULL, RG_DIALOG_FLAG_NORMAL, &prebuild_cache_cb} },
        RG_MENU_ENTRY_END,
    };
    memcpy(dest, entries, sizeof(entries));
}

void app_main(void)
{
    const rg_handlers_t handlers = {
        .event = &event_handler,
        .options = &options_handler,
        .about = &about_handler,
        .tabbed_options = &tabbed_options_handler,
    };

    app = rg_system_init(32000, &handlers, NULL);
    app->configNs = "launcher";
    app->isLauncher = true;

    RG_LOGI("Hardware marker: 0x%08X -> %s", (unsigned)rg_system_hw_marker(),
            rg_system_hw_marker_matches() ? "V3" : "V2 or unmarked");


    // Show boot animation (disabled - not working yet)
    // boot_animation();

    if (!rg_storage_ready())
    {
        rg_display_clear(C_SKY_BLUE);
        rg_gui_alert(_("SD Card Error"), _("Storage mount failed.\nUse a FAT32 or exFAT card."));
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
        rg_storage_mkdir(RG_BASE_PATH_ROMS "/gba");
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
