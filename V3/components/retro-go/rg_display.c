#include "rg_system.h"
#include "rg_display.h"
#include "rg_gui.h"   // stats overlay: rg_gui_draw_text from the display task

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LCD_BUFFER_LENGTH (RG_SCREEN_WIDTH * 4) // In pixels

// static rg_display_driver_t driver;
static rg_task_t *display_task_queue;
static rg_display_counters_t counters;
static rg_display_config_t config;
static rg_surface_t *osd;
static rg_surface_t *border;
static rg_display_t display;
static int16_t map_viewport_to_source_x[RG_SCREEN_WIDTH + 1];
static int16_t map_viewport_to_source_y[RG_SCREEN_HEIGHT + 1];
static uint32_t screen_line_checksum[RG_SCREEN_HEIGHT + 1];

#define LINE_IS_REPEATED(Y) (map_viewport_to_source_y[(Y)] == map_viewport_to_source_y[(Y) - 1])
// This is to avoid flooring a number that is approximated to .9999999 and be explicit about it
#define FLOAT_TO_INT(x) ((int)((x) + 0.1f))

static const char *SETTING_BACKLIGHT = "DispBacklight";
static const char *SETTING_VCOM = "DispVcom";
static const char *SETTING_VCOM_OFFSET = "DispVcomOff";
static const char *SETTING_VRH = "DispVrh";
static const char *SETTING_FRAMERATE = "DispFramerate";
static const char *SETTING_RGAIN = "DispRGain";
static const char *SETTING_GGAIN = "DispGGain";
static const char *SETTING_BGAIN = "DispBGain";
static const char *SETTING_CONTRAST = "DispContrast";
static const char *SETTING_GAMMA = "DispGamma";
static const char *SETTING_BATTERY_ICON = "BattIcon";
static const char *SETTING_MARGIN_L = "DispMargL";
static const char *SETTING_MARGIN_T = "DispMargT";
static const char *SETTING_MARGIN_R = "DispMargR";
static const char *SETTING_MARGIN_B = "DispMargB";
#define DEFAULT_VCOM     0x34     // Manufacturer INI value (0xBB VCOMS)
#define DEFAULT_VCOM_OFFSET 0x20  // ST7789 power-on default (0xC5 not in manufacturer INI)
#define DEFAULT_VRH         0x0F  // Manufacturer INI value (0xC3)
#define DEFAULT_FRAMERATE   0x02  // ~105Hz. Manufacturer INI is 0x0F (60Hz) but tearing was judged worse than the brightness/color cost (P3 panel, 2026-07)
#define DEFAULT_GAIN     100
#define DEFAULT_CONTRAST 100
#define DEFAULT_GAMMA    100
#define MIN_GAIN         25
#define MAX_GAIN         200
#define MIN_GAMMA        50
#define MAX_GAMMA        200
static const char *SETTING_SCALING = "DispScaling";
static const char *SETTING_OSD_STATS = "DispStats";
static const char *SETTING_FILTER = "DispFilter";
static const char *SETTING_ROTATION = "DispRotation";
static const char *SETTING_DISPLAY_TYPE = "DispType";
static const char *SETTING_BORDER = "DispBorder";
static const char *SETTING_CUSTOM_ZOOM = "DispCustomZoom";

static void lcd_init(void);
static void lcd_deinit(void);
static void lcd_sync(void);
static void lcd_set_rotation(int rotation);
static void lcd_set_backlight(float percent);
static void lcd_set_window(int left, int top, int width, int height);
static inline uint16_t *lcd_get_buffer(size_t length);
static inline void lcd_send_buffer(uint16_t *buffer, size_t length);
static void lcd_reinit_display(int type);

// When true, this boot is a showcase game-to-game transition: the panel already
// holds the loading image from before the reboot, so lcd_init()/lcd_reinit_display()
// must skip the reset/init (which would wipe it) and keep the backlight on. Set from
// the "ScPreserve" NVS flag in rg_display_init().
static bool display_preserve = false;

// False until rg_display_init() has populated `config` from the settings. Guards
// rg_display_set_backlight() against callers that run before init (see there).
static bool display_initialized = false;

// When true, rg_display_submit() drops frames. Set during shutdown so the running
// game can't push a last frame that tears through (or replaces) the loading screen.
static bool display_submit_blocked = false;

// On-screen low-battery icon. Off by default: this board blinks the status LED
// for the same condition, so showing both is redundant. Boards without that LED
// can switch it back on in Settings. Cached here rather than read per frame --
// draw_on_screen_display() runs on the display task and must not take the
// settings lock on every frame.
static bool osd_battery_icon = false;

#if RG_SCREEN_DRIVER == 0 /* ILI9341/ST7789 */
#include "drivers/display/ili9341.h"
#elif RG_SCREEN_DRIVER == 99
#include "drivers/display/sdl2.h"
#else
#include "drivers/display/dummy.h"
#endif

#ifdef ESP_PLATFORM
#include <driver/usb_serial_jtag.h>

// Reads lines from USB-JTAG serial (Arduino IDE / idf.py monitor) and dispatches
// display commands.
// Commands:  screen1 = ILI9341,  screen2 = ST7789,  screentype = print active type,
//            scanline = GSCAN (0x45) software-TE feasibility test.
static void serial_console_task(void *arg)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    // Install returns ESP_ERR_INVALID_STATE when IDF already installed it for console
    // output — that is fine, RX still works.
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        RG_LOGE("serial_console: driver init failed (%d)\n", err);
        vTaskDelete(NULL);
        return;
    }

    char line[32];
    int pos = 0;
    uint8_t b;

    while (true) {
        if (usb_serial_jtag_read_bytes(&b, 1, pdMS_TO_TICKS(100)) <= 0)
            continue;

        if (b == '\n' || b == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                if (strcmp(line, "screen1") == 0) {
                    RG_LOGI("serial: switching to ILI9341\n");
                    rg_display_set_type(RG_DISPLAY_TYPE_ILI9341);
                } else if (strcmp(line, "screen2") == 0) {
                    RG_LOGI("serial: switching to ST7789\n");
                    rg_display_set_type(RG_DISPLAY_TYPE_ST7789);
                } else if (strcmp(line, "screentype") == 0) {
                    RG_LOGI("screentype: %s\n",
                        rg_display_get_type() == RG_DISPLAY_TYPE_ILI9341 ? "ILI9341 (screen1)" : "ST7789 (screen2)");
                } else if (strcmp(line, "scanline") == 0) {
                    rg_display_gscan_test();
                }
            }
            pos = 0;
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)b;
        }
    }
}

// Per-core load for the stats overlay via FreeRTOS idle hooks: count idle-loop iterations
// per core and normalize on the highest idle RATE ever seen (self-calibrating; the menu —
// where the overlay gets enabled — is nearly idle and calibrates immediately). Heuristic
// but needs no config change (real runtime stats require FREERTOS_GENERATE_RUN_TIME_STATS).
#include "esp_freertos_hooks.h"
static volatile uint32_t osd_idle_cnt[2];
static bool osd_idle_hook0(void) { osd_idle_cnt[0]++; return true; }
static bool osd_idle_hook1(void) { osd_idle_cnt[1]++; return true; }
#endif

static int draw_on_screen_display(int region_start, int region_end)
{
    static unsigned int area_dirty = 0;
    int left = display.screen.width - 28;
    int top = 4;
    int border = 3;
    int width = 20;
    int height = 14;

    if (region_end < top + height)
        return top + height;

    if (osd_battery_icon && rg_system_get_indicator(RG_INDICATOR_POWER_LOW) && ((counters.totalFrames / 20) & 1))
    {
        rg_display_clear_rect(left, top, width, height, C_RED);
        rg_display_clear_rect(left + width, top + height / 4, border, height / 2, C_RED);
        rg_display_clear_rect(left + border, top + border, width - border * 2, height - border * 2, C_BLACK);
        area_dirty |= (1 << RG_INDICATOR_POWER_LOW);
    }
    else if (area_dirty)
    {
        if (display.viewport.width < display.screen.width || display.viewport.height < display.screen.height)
            rg_display_clear_rect(left, top, width + border, height, C_BLACK);
        memset(&screen_line_checksum[top], 0, sizeof(uint32_t) * height);
        area_dirty = 0;
    }
    return 0;
}

static inline unsigned blend_pixels(unsigned a, unsigned b)
{
    // Fast path (taken 80-90% of the time)
    if (a == b)
        return a;

    // Not the original author, but a good explanation is found at:
    // https://medium.com/@luc.trudeau/fast-averaging-of-high-color-16-bit-pixels-cb4ac7fd1488
    a = (a << 8) | (a >> 8);
    b = (b << 8) | (b >> 8);
    unsigned s = a ^ b;
    unsigned v = ((s & 0xF7DEU) >> 1) + (a & b) + (s & 0x0821U);
    return (v << 8) | (v >> 8);

    // This is my attempt at averaging two 565BE values without swapping bytes (3x the speed of the code above)
    // return (((a ^ b) & 0b1101111011110110U) >> 1) + (a & b);
}

static inline void write_update(const rg_surface_t *update)
{
    const int64_t time_start = rg_system_timer();

    bool filter_x = display.viewport.filter_x;
    bool filter_y = display.viewport.filter_y;
    int draw_left = display.viewport.left;
    int draw_top = display.viewport.top;
    int draw_width = display.viewport.width;
    int draw_height = display.viewport.height;

    // Defensive: refuse to render zero-sized viewport. Hit if update_viewport_scaling
    // ran with screen.width=0/height=0 (margins consumed everything) before the
    // clamp above existed, or if a race left stale state.
    if (draw_width <= 0 || draw_height <= 0)
        return;

    int crop_left = 0;
    int crop_top = 0;

    if (draw_left < 0)
    {
        crop_left += -draw_left * display.viewport.step_x;
        draw_width += draw_left * 2;
        draw_left = 0;
    }

    if (draw_top < 0)
    {
        crop_top += -draw_top * display.viewport.step_y;
        draw_height += draw_top * 2;
        draw_top = 0;
    }

    const int format = update->format;
    const int stride = update->stride;
    const void *data = update->data + update->offset + (crop_top * stride) + (crop_left * RG_PIXEL_GET_SIZE(format));
    const uint16_t *palette = update->palette;

    const bool partial_update = RG_SCREEN_PARTIAL_UPDATES;

    int lines_per_buffer = LCD_BUFFER_LENGTH / draw_width;
    int lines_remaining = draw_height;
    int lines_updated = 0;
    int window_top = -1;
    int osd_next_call = 20;

    for (int y = 0; y < draw_height;)
    {
        int lines_to_copy = RG_MIN(lines_per_buffer, lines_remaining);

        if (lines_to_copy < 1)
            break;

        // The vertical filter requires a block to start and end with unscaled lines
        if (filter_y)
        {
            while (lines_to_copy > 1 && (LINE_IS_REPEATED(y + lines_to_copy - 1) ||
                                         LINE_IS_REPEATED(y + lines_to_copy)))
                --lines_to_copy;
        }

        uint16_t *line_buffer = lcd_get_buffer(LCD_BUFFER_LENGTH);
        uint16_t *line_buffer_ptr = line_buffer;

        uint32_t checksum = 0xFFFFFFFF;
        bool need_update = !partial_update;

        for (int i = 0; i < lines_to_copy; ++i)
        {
            if (i > 0 && LINE_IS_REPEATED(y))
            {
                memcpy(line_buffer_ptr, line_buffer_ptr - draw_width, draw_width * 2);
                line_buffer_ptr += draw_width;
            }
            else
            {
                #define RENDER_LINE(PTR_TYPE, PIXEL) { \
                    PTR_TYPE *buffer = (PTR_TYPE *)(data + map_viewport_to_source_y[y] * stride);\
                    for (int xx = 0; xx < draw_width; ++xx) { \
                        int x = map_viewport_to_source_x[xx]; \
                        *line_buffer_ptr++ = (PIXEL); \
                    } \
                }
                if (format & RG_PIXEL_PALETTE)
                    RENDER_LINE(uint8_t, palette[buffer[x]])
                else if (format == RG_PIXEL_565_LE)
                    RENDER_LINE(uint16_t, (buffer[x] << 8) | (buffer[x] >> 8))
                else
                    RENDER_LINE(uint16_t, buffer[x])

                if (partial_update)
                {
                    checksum = rg_hash((void*)(line_buffer_ptr - draw_width), draw_width * 2);
                }
            }

            if (screen_line_checksum[draw_top + y] != checksum)
            {
                screen_line_checksum[draw_top + y] = checksum;
                need_update = true;
            }

            ++y;
        }

        if (filter_x && need_update)
        {
            for (int i = 0; i < lines_to_copy; ++i)
            {
                uint16_t *buffer = line_buffer + i * draw_width;
                for (int x = 1; x < draw_width - 1; ++x)
                {
                    if (map_viewport_to_source_x[x] == map_viewport_to_source_x[x - 1])
                    {
                        buffer[x] = blend_pixels(buffer[x - 1], buffer[x + 1]);
                    }
                }
            }
        }

        if (filter_y && need_update)
        {
            int top = y - lines_to_copy;
            for (int i = 1; i < lines_to_copy - 1; ++i)
            {
                if (LINE_IS_REPEATED(top + i))
                {
                    uint16_t *lineA = line_buffer + (i - 1) * draw_width;
                    uint16_t *lineB = line_buffer + (i + 0) * draw_width;
                    uint16_t *lineC = line_buffer + (i + 1) * draw_width;
                    for (size_t x = 0; x < draw_width; ++x)
                    {
                        lineB[x] = blend_pixels(lineA[x], lineC[x]);
                    }
                }
            }
        }

        if (need_update)
        {
            int left = display.screen.margins.left + draw_left;
            int top = display.screen.margins.top + draw_top + y - lines_to_copy;
            if (top != window_top)
                lcd_set_window(left, top, draw_width, lines_remaining);
            lcd_send_buffer(line_buffer, draw_width * lines_to_copy);
            window_top = top + lines_to_copy;
            lines_updated += lines_to_copy;
        }
        else
        {
            // Return unused buffer
            lcd_send_buffer(line_buffer, 0);
        }

        // Drawing the OSD as we progress reduces flicker compared to doing it once at the end
        if (osd_next_call && draw_top + y >= osd_next_call)
        {
            osd_next_call = draw_on_screen_display(0, draw_top + y);
            window_top = -1;
        }

        lines_remaining -= lines_to_copy;
    }

    if (osd != NULL)
    {
        // TODO: Draw on screen display. By default it should be bottom left which is fine
        // for both virtual keyboard and info labels. Maybe make it configurable later...
    }

    // Stats overlay (bottom-left border): core/fps/busy/frameskip/free memory, refreshed
    // once per second. Runs on the display task so all LCD access stays serialized.
    // Meant for Scaling=Off (border is free there); with other scaling modes the game
    // partially overwrites the text each frame (blinks but stays readable). Self-heals
    // within 1s after a menu wiped the border.
    {
        static int64_t stats_next = 0;
        static rg_rect_t stats_rect;   // last drawn rect (for precise wiping)
        if (config.osd_stats)
        {
            int64_t now = rg_system_timer();
            if (now >= stats_next)
            {
                stats_next = now + 1000000;
                rg_stats_t st = rg_system_get_counters();
                const rg_app_t *app = rg_system_get_app();
                const char *tag = rg_display_osd_tag();
                char fs[16];
                if (app->frameskip < 0)      strcpy(fs, "off");
                else if (app->frameskip == 0) strcpy(fs, "auto");
                else                          snprintf(fs, sizeof(fs), "%d", app->frameskip);
                char c0s[8] = "--", c1s[8] = "--";
#ifdef ESP_PLATFORM
                {
                    static bool hooked = false;
                    static int64_t idle_t0 = 0;
                    static uint32_t idle_base = 0;   // highest idle rate ever = ~0% load
                    if (!hooked)
                    {
                        esp_register_freertos_idle_hook_for_cpu(osd_idle_hook0, 0);
                        esp_register_freertos_idle_hook_for_cpu(osd_idle_hook1, 1);
                        hooked = true;
                        idle_t0 = now;
                        osd_idle_cnt[0] = osd_idle_cnt[1] = 0;
                    }
                    else if (now > idle_t0)
                    {
                        // Rate instead of raw count: the interval varies (menus pause frames).
                        uint32_t r0 = (uint32_t)((uint64_t)osd_idle_cnt[0] * 1000000 / (now - idle_t0));
                        uint32_t r1 = (uint32_t)((uint64_t)osd_idle_cnt[1] * 1000000 / (now - idle_t0));
                        osd_idle_cnt[0] = osd_idle_cnt[1] = 0;
                        idle_t0 = now;
                        uint32_t m = RG_MAX(r0, r1);
                        if (m > idle_base) idle_base = m;
                        if (idle_base)
                        {
                            snprintf(c0s, sizeof(c0s), "%d%%", 100 - (int)RG_MIN(100, r0 * 100 / idle_base));
                            snprintf(c1s, sizeof(c1s), "%d%%", 100 - (int)RG_MIN(100, r1 * 100 / idle_base));
                        }
                    }
                }
#endif
                char buf[80];
                snprintf(buf, sizeof(buf), "%s | %.0ffps | C0 %s | C1 %s | fs %s",
                         tag ? tag : "?", st.totalFPS, c0s, c1s, fs);
                rg_rect_t r = rg_gui_draw_text(RG_GUI_CENTER, 2, 0, buf, C_WHITE, C_BLACK,
                                               RG_TEXT_NO_SYNC);
                // Centered = x shifts when the width changes: wipe the flanks of the
                // previous text that fall outside the new one.
                if (stats_rect.width > 0)
                {
                    if (stats_rect.left < r.left)
                        rg_display_clear_rect(stats_rect.left, stats_rect.top,
                                              r.left - stats_rect.left, stats_rect.height, C_BLACK);
                    int prev_r = stats_rect.left + stats_rect.width, new_r = r.left + r.width;
                    if (prev_r > new_r)
                        rg_display_clear_rect(new_r, stats_rect.top, prev_r - new_r,
                                              stats_rect.height, C_BLACK);
                }
                stats_rect = r;
            }
        }
        else if (stats_rect.width)
        {
            rg_display_clear_rect(stats_rect.left, stats_rect.top,
                                  stats_rect.width, stats_rect.height, C_BLACK);
            stats_rect = (rg_rect_t){0, 0, 0, 0};
            stats_next = 0;
        }
    }

    if (lines_updated > draw_height * 0.80f)
        counters.fullFrames++;
    else
        counters.partFrames++;
    counters.busyTime += rg_system_timer() - time_start;
}

static void update_viewport_scaling(void)
{
    int screen_width = display.screen.width;
    int screen_height = display.screen.height;
    int src_width = display.source.width;
    int src_height = display.source.height;
    int new_width = src_width;
    int new_height = src_height;

    if (config.scaling == RG_DISPLAY_SCALING_FULL)
    {
        new_width = screen_width;
        new_height = screen_height;
    }
    else if (config.scaling == RG_DISPLAY_SCALING_FIT)
    {
        new_width = FLOAT_TO_INT(screen_height * ((float)src_width / src_height));
        new_height = screen_height;
        if (new_width > screen_width) {
            new_width = screen_width;
            new_height = FLOAT_TO_INT(screen_width * ((float)src_height / src_width));
        }
    }
    else if (config.scaling == RG_DISPLAY_SCALING_ZOOM)
    {
        new_width = FLOAT_TO_INT(src_width * config.custom_zoom);
        new_height = FLOAT_TO_INT(src_height * config.custom_zoom);
    }

    // Everything works better when we use even dimensions!
    new_width &= ~1;
    new_height &= ~1;

    // Defensive: ensure viewport is at least 2x2 to prevent downstream divide-by-zero
    // (write_update has `LCD_BUFFER_LENGTH / draw_width` and similar that explode at 0).
    // Can happen with tiny zoom factors or when margins consume all available space.
    if (new_width < 2)  new_width  = 2;
    if (new_height < 2) new_height = 2;

    display.viewport.left = (screen_width - new_width) / 2;
    display.viewport.top = (screen_height - new_height) / 2;
    display.viewport.width = new_width;
    display.viewport.height = new_height;

    display.viewport.step_x = (float)src_width / display.viewport.width;
    display.viewport.step_y = (float)src_height / display.viewport.height;

    display.viewport.filter_x = (config.filter == RG_DISPLAY_FILTER_HORIZ || config.filter == RG_DISPLAY_FILTER_BOTH) &&
                                (config.scaling && (display.viewport.width % src_width) != 0);
    display.viewport.filter_y = (config.filter == RG_DISPLAY_FILTER_VERT || config.filter == RG_DISPLAY_FILTER_BOTH) &&
                                (config.scaling && (display.viewport.height % src_height) != 0);

    memset(screen_line_checksum, 0, sizeof(screen_line_checksum));

    for (int x = 0; x < screen_width; ++x)
        map_viewport_to_source_x[x] = FLOAT_TO_INT(x * display.viewport.step_x);
    for (int y = 0; y < screen_height; ++y)
        map_viewport_to_source_y[y] = FLOAT_TO_INT(y * display.viewport.step_y);

    RG_LOGI("%dx%d@%.3f => %dx%d@%.3f left:%d top:%d step_x:%.2f step_y:%.2f", src_width, src_height,
            (float)src_width / src_height, new_width, new_height, (float)new_width / new_height,
            display.viewport.left, display.viewport.top, display.viewport.step_x, display.viewport.step_y);
}

static bool load_border_file(const char *filename)
{
    RG_LOGI("Loading border file: %s", filename ?: "(none)");

    free(border), border = NULL;
    display.changed = true;

    if (filename && (border = rg_surface_load_image_file(filename, 0)))
    {
        if (border->width != rg_display_get_width() || border->height != rg_display_get_height())
        {
            rg_surface_t *resized = rg_surface_resize(border, rg_display_get_width(), rg_display_get_height());
            if (resized)
            {
                rg_surface_free(border);
                border = resized;
            }
        }
        return true;
    }
    return false;
}

IRAM_ATTR
static void display_task(void *arg)
{
    rg_task_msg_t msg;

    while (rg_task_peek(&msg))
    {
        // Received a shutdown request!
        if (msg.type == RG_TASK_MSG_STOP)
            break;

        if (display.changed)
        {
            update_viewport_scaling();
            // Clear the screen if the viewport doesn't cover the entire screen because garbage could remain on the sides
            if (display.viewport.width < display.screen.width || display.viewport.height < display.screen.height)
            {
                if (border)
                    rg_display_write_rect(0, 0, border->width, border->height, 0, border->data, RG_DISPLAY_WRITE_NOSYNC);
                else
                    rg_display_clear_except(display.viewport.left, display.viewport.top, display.viewport.width, display.viewport.height, C_BLACK);
            }
            display.changed = false;
        }

        write_update(msg.dataPtr);

        rg_task_receive(&msg);

        lcd_sync();
    }
}

void rg_display_force_redraw(void)
{
    display.changed = true;
    // memset(screen_line_checksum, 0, sizeof(screen_line_checksum));
    rg_system_event(RG_EVENT_REDRAW, NULL);
    rg_display_sync(true);
}

const rg_display_t *rg_display_get_info(void)
{
    return &display;
}

rg_display_counters_t rg_display_get_counters(void)
{
    return counters;
}

int rg_display_get_width(void)
{
    // return display.screen.real_width - (display.screen.margins.left + display.screen.margins.right);
    return display.screen.width;
}

int rg_display_get_height(void)
{
    // return display.screen.real_height - (display.screen.margins.top + display.screen.margins.bottom);
    return display.screen.height;
}

void rg_display_set_scaling(display_scaling_t scaling)
{
    config.scaling = RG_MIN(RG_MAX(0, scaling), RG_DISPLAY_SCALING_COUNT - 1);
    rg_settings_set_number(NS_APP, SETTING_SCALING, config.scaling);
    display.changed = true;
}

display_scaling_t rg_display_get_scaling(void)
{
    return config.scaling;
}

// Optionally implemented by the emulator: short label prefixed to the stats overlay
// (e.g. "jit"/"cla"). Weak default = no label.
__attribute__((weak)) const char *rg_display_osd_tag(void)
{
    return NULL;
}

void rg_display_set_osd_stats(int enabled)
{
    config.osd_stats = enabled ? 1 : 0;
    rg_settings_set_number(NS_APP, SETTING_OSD_STATS, config.osd_stats);
}

int rg_display_get_osd_stats(void)
{
    return config.osd_stats;
}

void rg_display_set_custom_zoom(double factor)
{
    config.custom_zoom = RG_MIN(RG_MAX(0.1, factor), 2.0);
    rg_settings_set_number(NS_APP, SETTING_CUSTOM_ZOOM, config.custom_zoom);
    display.changed = true;
}

double rg_display_get_custom_zoom(void)
{
    return config.custom_zoom;
}

void rg_display_set_filter(display_filter_t filter)
{
    config.filter = RG_MIN(RG_MAX(0, filter), RG_DISPLAY_FILTER_COUNT - 1);
    rg_settings_set_number(NS_APP, SETTING_FILTER, config.filter);
    display.changed = true;
}

display_filter_t rg_display_get_filter(void)
{
    return config.filter;
}

void rg_display_set_rotation(display_rotation_t rotation)
{
    config.rotation = RG_MIN(RG_MAX(0, rotation), RG_DISPLAY_ROTATION_COUNT - 1);
    rg_settings_set_number(NS_APP, SETTING_ROTATION, config.rotation);
    display.changed = true;
}

display_rotation_t rg_display_get_rotation(void)
{
    return config.rotation;
}

void rg_display_set_type(display_type_t type)
{
    type = RG_MIN(RG_MAX(0, type), RG_DISPLAY_TYPE_COUNT - 1);
    rg_settings_set_number(NS_GLOBAL, SETTING_DISPLAY_TYPE, type);
    rg_settings_commit();
    // Drain any pending DMA before reinitialising the controller.
    rg_display_sync(true);
    lcd_reinit_display((int)type);
    // Reapply software color pipeline (LUTs survive the reinit, but these panel registers don't).
    lcd_set_vcom((uint8_t)rg_settings_get_number(NS_GLOBAL, SETTING_VCOM, DEFAULT_VCOM));
    lcd_set_vcom_offset((uint8_t)rg_settings_get_number(NS_GLOBAL, SETTING_VCOM_OFFSET, DEFAULT_VCOM_OFFSET));
    lcd_set_vrh((uint8_t)rg_settings_get_number(NS_GLOBAL, SETTING_VRH, DEFAULT_VRH));
    lcd_set_framerate((uint8_t)rg_settings_get_number(NS_GLOBAL, SETTING_FRAMERATE, DEFAULT_FRAMERATE));
    rg_display_clear(C_BLACK);
    display.changed = true;
}

display_type_t rg_display_get_type(void)
{
    return (display_type_t)rg_settings_get_number(NS_GLOBAL, SETTING_DISPLAY_TYPE, RG_DISPLAY_TYPE_ST7789);
}

// GSCAN (0x45) scanline-read feasibility test; results go to the console log.
// Frame submission is paused so the bit-banged transfer can't interleave with DMA.
void rg_display_gscan_test(void)
{
    display_submit_blocked = true;
    rg_usleep(100 * 1000); // let any in-flight frame drain
#if RG_SCREEN_DRIVER == 0
    lcd_gscan_test();
#else
    RG_LOGE("GSCAN test only implemented for the ILI9341/ST7789 SPI driver\n");
#endif
    display_submit_blocked = false;
}

void rg_display_set_battery_icon(bool enable)
{
    osd_battery_icon = enable;
    rg_settings_set_number(NS_GLOBAL, SETTING_BATTERY_ICON, enable);
    display.changed = true; // let the next frame clear the icon's area if needed
}

bool rg_display_get_battery_icon(void)
{
    return osd_battery_icon;
}

void rg_display_set_backlight(display_backlight_t percent)
{
    // Before rg_display_init() `config` is still all zeroes, so a caller that does
    // read-modify-write on rg_display_get_backlight() (the brightness wheel in the
    // input task, which is started earlier in rg_system_init) would compute its new
    // value from 0 and persist it. rg_display_init() then loads that value and the
    // panel comes up at 1%: everything boots, but the screen looks dead. Ignore
    // such calls -- the stored setting is the truth until init has read it.
    if (!display_initialized)
    {
        RG_LOGW("Ignoring backlight change (%d%%) before display init\n", (int)percent);
        return;
    }
    config.backlight = RG_MIN(RG_MAX(percent, RG_DISPLAY_BACKLIGHT_MIN), RG_DISPLAY_BACKLIGHT_MAX);
    rg_settings_set_number(NS_GLOBAL, SETTING_BACKLIGHT, config.backlight);
    lcd_set_backlight(config.backlight);
}

display_backlight_t rg_display_get_backlight(void)
{
    return config.backlight;
}

void rg_display_backlight_off(void)
{
    lcd_set_backlight(0); // does not touch config/NVS, so the setting is preserved
}

void rg_display_set_vcom(uint8_t value)
{
    if (value > 0x3F) value = 0x3F;
    rg_settings_set_number(NS_GLOBAL, SETTING_VCOM, value);
    lcd_set_vcom(value);
}

uint8_t rg_display_get_vcom(void)
{
    return (uint8_t)rg_settings_get_number(NS_GLOBAL, SETTING_VCOM, DEFAULT_VCOM);
}

void rg_display_set_vcom_offset(uint8_t value)
{
    rg_settings_set_number(NS_GLOBAL, SETTING_VCOM_OFFSET, value);
    lcd_set_vcom_offset(value);
}

uint8_t rg_display_get_vcom_offset(void)
{
    return (uint8_t)rg_settings_get_number(NS_GLOBAL, SETTING_VCOM_OFFSET, DEFAULT_VCOM_OFFSET);
}

void rg_display_set_vrh(uint8_t value)
{
    if (value > 0x3F) value = 0x3F;
    rg_settings_set_number(NS_GLOBAL, SETTING_VRH, value);
    lcd_set_vrh(value);
}

uint8_t rg_display_get_vrh(void)
{
    return (uint8_t)rg_settings_get_number(NS_GLOBAL, SETTING_VRH, DEFAULT_VRH);
}

void rg_display_set_framerate(uint8_t value)
{
    if (value > 0x1F) value = 0x1F;
    rg_settings_set_number(NS_GLOBAL, SETTING_FRAMERATE, value);
    lcd_set_framerate(value);
}

uint8_t rg_display_get_framerate(void)
{
    return (uint8_t)rg_settings_get_number(NS_GLOBAL, SETTING_FRAMERATE, DEFAULT_FRAMERATE);
}

static int clamp_gain(int v)
{
    if (v < MIN_GAIN) return MIN_GAIN;
    if (v > MAX_GAIN) return MAX_GAIN;
    return v;
}

static int clamp_gamma(int v)
{
    if (v < MIN_GAMMA) return MIN_GAMMA;
    if (v > MAX_GAMMA) return MAX_GAMMA;
    return v;
}

static void apply_all_color(int r, int g, int b)
{
    int contrast = rg_settings_get_number(NS_GLOBAL, SETTING_CONTRAST, DEFAULT_CONTRAST);
    int gamma = rg_settings_get_number(NS_GLOBAL, SETTING_GAMMA, DEFAULT_GAMMA);
    lcd_set_color_config(r, g, b, contrast, gamma);
}

void rg_display_set_color_gains(int r_pct, int g_pct, int b_pct)
{
    r_pct = clamp_gain(r_pct);
    g_pct = clamp_gain(g_pct);
    b_pct = clamp_gain(b_pct);
    rg_settings_set_number(NS_GLOBAL, SETTING_RGAIN, r_pct);
    rg_settings_set_number(NS_GLOBAL, SETTING_GGAIN, g_pct);
    rg_settings_set_number(NS_GLOBAL, SETTING_BGAIN, b_pct);
    apply_all_color(r_pct, g_pct, b_pct);
}

void rg_display_get_color_gains(int *r_pct, int *g_pct, int *b_pct)
{
    if (r_pct) *r_pct = rg_settings_get_number(NS_GLOBAL, SETTING_RGAIN, DEFAULT_GAIN);
    if (g_pct) *g_pct = rg_settings_get_number(NS_GLOBAL, SETTING_GGAIN, DEFAULT_GAIN);
    if (b_pct) *b_pct = rg_settings_get_number(NS_GLOBAL, SETTING_BGAIN, DEFAULT_GAIN);
}

void rg_display_set_contrast(int pct)
{
    pct = clamp_gain(pct);
    rg_settings_set_number(NS_GLOBAL, SETTING_CONTRAST, pct);
    int r, g, b;
    rg_display_get_color_gains(&r, &g, &b);
    apply_all_color(r, g, b);
}

int rg_display_get_contrast(void)
{
    return rg_settings_get_number(NS_GLOBAL, SETTING_CONTRAST, DEFAULT_CONTRAST);
}

void rg_display_set_gamma(int pct)
{
    pct = clamp_gamma(pct);
    rg_settings_set_number(NS_GLOBAL, SETTING_GAMMA, pct);
    int r, g, b;
    rg_display_get_color_gains(&r, &g, &b);
    apply_all_color(r, g, b);
}

int rg_display_get_gamma(void)
{
    return rg_settings_get_number(NS_GLOBAL, SETTING_GAMMA, DEFAULT_GAMMA);
}

void rg_display_set_margins(int left, int top, int right, int bottom)
{
    int max_h = RG_SCREEN_WIDTH  / 4;
    int max_v = RG_SCREEN_HEIGHT / 4;
    if (left   < 0) left   = 0; else if (left   > max_h) left   = max_h;
    if (right  < 0) right  = 0; else if (right  > max_h) right  = max_h;
    if (top    < 0) top    = 0; else if (top    > max_v) top    = max_v;
    if (bottom < 0) bottom = 0; else if (bottom > max_v) bottom = max_v;

    int old_l = display.screen.margins.left;
    int old_t = display.screen.margins.top;
    int old_r = display.screen.margins.right;
    int old_b = display.screen.margins.bottom;

    rg_settings_set_number(NS_GLOBAL, SETTING_MARGIN_L, left);
    rg_settings_set_number(NS_GLOBAL, SETTING_MARGIN_T, top);
    rg_settings_set_number(NS_GLOBAL, SETTING_MARGIN_R, right);
    rg_settings_set_number(NS_GLOBAL, SETTING_MARGIN_B, bottom);

    display.screen.margins.left   = left;
    display.screen.margins.top    = top;
    display.screen.margins.right  = right;
    display.screen.margins.bottom = bottom;
    display.screen.width  = RG_SCREEN_WIDTH  - left - right;
    display.screen.height = RG_SCREEN_HEIGHT - top  - bottom;
    display.changed = true;

    if (left   > old_l) rg_display_clear_rect(-(left - old_l), -top,        left   - old_l, RG_SCREEN_HEIGHT, C_BLACK);
    if (right  > old_r) rg_display_clear_rect(display.screen.width, -top,   right  - old_r, RG_SCREEN_HEIGHT, C_BLACK);
    if (top    > old_t) rg_display_clear_rect(-left, -(top - old_t),        RG_SCREEN_WIDTH, top    - old_t,  C_BLACK);
    if (bottom > old_b) rg_display_clear_rect(-left, display.screen.height, RG_SCREEN_WIDTH, bottom - old_b,  C_BLACK);
}

void rg_display_get_margins(int *left, int *top, int *right, int *bottom)
{
    if (left)   *left   = rg_settings_get_number(NS_GLOBAL, SETTING_MARGIN_L, 0);
    if (top)    *top    = rg_settings_get_number(NS_GLOBAL, SETTING_MARGIN_T, 0);
    if (right)  *right  = rg_settings_get_number(NS_GLOBAL, SETTING_MARGIN_R, 0);
    if (bottom) *bottom = rg_settings_get_number(NS_GLOBAL, SETTING_MARGIN_B, 0);
}

void rg_display_set_border(const char *filename)
{
    free(config.border_file);
    config.border_file = NULL;

    if (load_border_file(filename))
    {
        rg_settings_set_string(NS_APP, SETTING_BORDER, filename);
        config.border_file = strdup(filename);
    }
    else
    {
        rg_settings_set_string(NS_APP, SETTING_BORDER, NULL);
        config.border_file = NULL;
    }
    display.changed = true;
}

char *rg_display_get_border(void)
{
    return rg_settings_get_string(NS_APP, SETTING_BORDER, NULL);
}

void rg_display_submit(const rg_surface_t *update, uint32_t flags)
{
    const int64_t time_start = rg_system_timer();

    // Shutting down: ignore further frames so the game can't tear through / overwrite
    // the loading screen during a transition.
    if (display_submit_blocked)
        return;

    // Those things should probably be asserted, but this is a new system let's be forgiving...
    if (!update || !update->data)
        return;

    if (display.source.width != update->width || display.source.height != update->height)
    {
        rg_display_sync(true);
        display.source.width = update->width;
        display.source.height = update->height;
        display.changed = true;
    }

    rg_task_send(display_task_queue, &(rg_task_msg_t){.dataPtr = update});

    counters.blockTime += rg_system_timer() - time_start;
    counters.totalFrames++;
}

bool rg_display_sync(bool block)
{
    while (block && rg_task_messages_waiting(display_task_queue))
        continue; // We should probably yield?
    return !rg_task_messages_waiting(display_task_queue);
}

void rg_display_block_submit(void)
{
    display_submit_blocked = true; // drop any further game frames
    rg_display_sync(true);         // let the in-flight frame finish so it can't tear
}

void rg_display_write_rect(int left, int top, int width, int height, int stride, const uint16_t *buffer, uint32_t flags)
{
    RG_ASSERT_ARG(buffer);

    // calc stride before clipping width
    stride = RG_MAX(stride, width * 2);

    // Clipping
    width = RG_MIN(width, display.screen.width - left);
    height = RG_MIN(height, display.screen.height - top);

    // This can happen when left or top is out of bound (or hits the right/bottom edge
    // exactly after margin clipping). <=0 catches both — without the equals case,
    // width=0 falls through and triggers an IntegerDivideByZero on the
    // `LCD_BUFFER_LENGTH / width` below.
    if (width <= 0 || height <= 0)
        return;

    // This will work for now because we rarely draw from different threads (so all we need is ensure
    // that we're not interrupting a display update). But what we SHOULD be doing is acquire a lock
    // before every call to lcd_set_window and release it only after the last call to lcd_send_buffer.
    if (!(flags & RG_DISPLAY_WRITE_NOSYNC))
        rg_display_sync(true);

    // This isn't really necessary but it makes sense to invalidate
    // the lines we're about to overwrite...
    for (size_t y = 0; y < height; ++y)
        screen_line_checksum[top + y] = 0;

    lcd_set_window(left + display.screen.margins.left, top + display.screen.margins.top, width, height);

    for (size_t y = 0; y < height;)
    {
        uint16_t *lcd_buffer = lcd_get_buffer(LCD_BUFFER_LENGTH);
        size_t num_lines = RG_MIN(LCD_BUFFER_LENGTH / width, height - y);

        // Copy line by line because stride may not match width
        for (size_t line = 0; line < num_lines; ++line)
        {
            uint16_t *src = (void *)buffer + ((y + line) * stride);
            uint16_t *dst = lcd_buffer + (line * width);
            if (flags & RG_DISPLAY_WRITE_NOSWAP)
            {
                memcpy(dst, src, width * 2);
            }
            else
            {
                for (size_t i = 0; i < width; ++i)
                    dst[i] = (src[i] >> 8) | (src[i] << 8);
            }
        }

        lcd_send_buffer(lcd_buffer, width * num_lines);
        y += num_lines;
    }

    lcd_sync();
}

void rg_display_clear_rect(int left, int top, int width, int height, uint16_t color_le)
{
    const uint16_t color_be = (color_le << 8) | (color_le >> 8);
    int pixels_remaining = width * height;
    if (pixels_remaining > 0)
    {
        lcd_set_window(left + display.screen.margins.left, top + display.screen.margins.top, width, height);
        while (pixels_remaining > 0)
        {
            uint16_t *buffer = lcd_get_buffer(LCD_BUFFER_LENGTH);
            int pixels = RG_MIN(pixels_remaining, LCD_BUFFER_LENGTH);
            for (size_t j = 0; j < pixels; ++j)
                buffer[j] = color_be;
            lcd_send_buffer(buffer, pixels);
            pixels_remaining -= pixels;
        }
    }
}

void rg_display_clear_except(int left, int top, int width, int height, uint16_t color_le)
{
    // Clear everything on the real screen except the specified viewport area
    // The viewport coordinates (left, top, width, height) are relative to the visible area
    // We need to clear the entire real screen around the viewport

    // Calculate viewport position on the real screen
    int vp_real_left = left + display.screen.margins.left;
    int vp_real_top = top + display.screen.margins.top;
    int vp_real_right = vp_real_left + width;
    int vp_real_bottom = vp_real_top + height;

    int scr_width = display.screen.real_width;
    int scr_height = display.screen.real_height;

    // Offset to reach position 0,0 on the real screen (since clear_rect adds margins)
    int x_off = -display.screen.margins.left;
    int y_off = -display.screen.margins.top;

    // Left strip (full height of real screen)
    if (vp_real_left > 0)
        rg_display_clear_rect(x_off, y_off, vp_real_left, scr_height, color_le);

    // Right strip (full height of real screen)
    if (vp_real_right < scr_width)
        rg_display_clear_rect(x_off + vp_real_right, y_off, scr_width - vp_real_right, scr_height, color_le);

    // Top strip (only the width of viewport, between left and right strips)
    if (vp_real_top > 0)
        rg_display_clear_rect(x_off + vp_real_left, y_off, width, vp_real_top, color_le);

    // Bottom strip (only the width of viewport, between left and right strips)
    if (vp_real_bottom < scr_height)
        rg_display_clear_rect(x_off + vp_real_left, y_off + vp_real_bottom, width, scr_height - vp_real_bottom, color_le);
}

void rg_display_clear(uint16_t color_le)
{
    // Always wipe the full physical panel to black first. This preserves the
    // original hard-wipe guarantee (no stale content left behind, including in
    // the lens-cropped margin strips). Then fill only the visible area with the
    // requested color, so a colored clear never bleeds past the bezel/lens.
    // A black clear behaves exactly as before (whole panel ends up black).
    rg_display_clear_rect(-display.screen.margins.left, -display.screen.margins.top, display.screen.real_width,
                          display.screen.real_height, C_BLACK);
    if (color_le != C_BLACK)
        rg_display_clear_rect(0, 0, display.screen.width, display.screen.height, color_le);
    // Invalidate the partial-update cache so the next frame fully redraws.
    // Without this, write_update would skip lines whose new game pixels happen
    // to hash to the same value as the pre-clear content, leaving black bars
    // on screen until the game scrolls past them or a menu forces a repaint.
    // Symptom: black squares in-game after rg_netplay_quick_start() returns.
    memset(screen_line_checksum, 0, sizeof(screen_line_checksum));
}

void rg_display_deinit(void)
{
    display_initialized = false;
    rg_task_send(display_task_queue, &(rg_task_msg_t){.type = RG_TASK_MSG_STOP});
    // Blank the panel before teardown so reboot/re-init garbage stays hidden — but
    // NOT during a showcase transition, where we keep the loading image lit so it
    // stays visible across the reboot for a smooth switch.
    if (!rg_settings_get_number(NS_GLOBAL, "ScPreserve", 0))
        lcd_set_backlight(0);
    lcd_deinit();
    RG_LOGI("Display terminated.\n");
}

void rg_display_init(void)
{
    RG_LOGI("Initialization...\n");
    // TO DO: We probably should call the setters to ensure valid values...
    config = (rg_display_config_t){
        .backlight = rg_settings_get_number(NS_GLOBAL, SETTING_BACKLIGHT, 80),
        // (osd_battery_icon is read just below; it lives outside rg_display_config_t)
        .scaling = rg_settings_get_number(NS_APP, SETTING_SCALING, RG_DISPLAY_SCALING_FULL),
        .osd_stats = rg_settings_get_number(NS_APP, SETTING_OSD_STATS, 0),
        .filter = rg_settings_get_number(NS_APP, SETTING_FILTER, RG_DISPLAY_FILTER_BOTH),
        .rotation = rg_settings_get_number(NS_APP, SETTING_ROTATION, RG_DISPLAY_ROTATION_AUTO),
        .border_file = rg_settings_get_string(NS_APP, SETTING_BORDER, NULL),
        .custom_zoom = rg_settings_get_number(NS_APP, SETTING_CUSTOM_ZOOM, 1.0),
    };
    display = (rg_display_t){
        .screen.real_width = RG_SCREEN_WIDTH,
        .screen.real_height = RG_SCREEN_HEIGHT,
        .screen.width = RG_SCREEN_WIDTH,
        .screen.height = RG_SCREEN_HEIGHT,
        .screen.margins = RG_SCREEN_VISIBLE_AREA,
        .changed = true,
    };
    osd_battery_icon = rg_settings_get_number(NS_GLOBAL, SETTING_BATTERY_ICON, 0);

    // Override compile-time visible area with NVS values.
    display.screen.margins.left   = rg_settings_get_number(NS_GLOBAL, SETTING_MARGIN_L, display.screen.margins.left);
    display.screen.margins.top    = rg_settings_get_number(NS_GLOBAL, SETTING_MARGIN_T, display.screen.margins.top);
    display.screen.margins.right  = rg_settings_get_number(NS_GLOBAL, SETTING_MARGIN_R, display.screen.margins.right);
    display.screen.margins.bottom = rg_settings_get_number(NS_GLOBAL, SETTING_MARGIN_B, display.screen.margins.bottom);
    display.screen.width  -= display.screen.margins.left + display.screen.margins.right;
    display.screen.height -= display.screen.margins.top  + display.screen.margins.bottom;
    // Showcase transition? The panel still shows the loading image from before the
    // reboot; preserve it (skip reset/clear/reinit) for a smooth, flash-free switch.
    // Only valid after a SW restart: on a real power-off / deep-sleep wake the panel
    // was power-cycled, so we MUST do a full init or the screen stays white. Consume
    // the flag either way so a stale one (e.g. powered off mid-showcase) can't linger.
    // Also require showcase to actually be running: ScPreserve is only ever set by
    // a showcase game-to-game transition (see shutdown_cleanup). Without this check
    // a flag left behind by an aborted rotation makes EVERY later software reset
    // skip the panel reset and init entirely -- so a panel that failed to come up
    // can never be recovered by launching a game, only by pulling the power.
    display_preserve = rg_settings_get_number(NS_GLOBAL, "ScPreserve", 0)
                       && rg_settings_get_number(NS_GLOBAL, "DisplayActive", 0)
                       && !rg_system_get_app()->isColdBoot;
    rg_settings_set_number(NS_GLOBAL, "ScPreserve", 0);
    lcd_init();
    // If the user has selected a non-default screen type, reinit with the correct sequence.
    // lcd_init() always runs RG_SCREEN_INIT (ST7789, the default screen2); this
    // overrides it for ILI9341 (screen1). Skipped during a preserve transition so the
    // retained loading image isn't wiped (the panel already has the right type's config).
    if (!display_preserve)
    {
        int stored_type = rg_settings_get_number(NS_GLOBAL, SETTING_DISPLAY_TYPE, RG_DISPLAY_TYPE_ST7789);
        if (stored_type != RG_DISPLAY_TYPE_ST7789)
            lcd_reinit_display(stored_type);
    }
    // Apply user-tuned VCOM and the full software color pipeline (R/G/B gains, contrast, gamma).
    lcd_set_vcom((uint8_t)rg_settings_get_number(NS_GLOBAL, SETTING_VCOM, DEFAULT_VCOM));
    lcd_set_vcom_offset((uint8_t)rg_settings_get_number(NS_GLOBAL, SETTING_VCOM_OFFSET, DEFAULT_VCOM_OFFSET));
    lcd_set_vrh((uint8_t)rg_settings_get_number(NS_GLOBAL, SETTING_VRH, DEFAULT_VRH));
    lcd_set_framerate((uint8_t)rg_settings_get_number(NS_GLOBAL, SETTING_FRAMERATE, DEFAULT_FRAMERATE));
    lcd_set_color_config(
        rg_settings_get_number(NS_GLOBAL, SETTING_RGAIN, DEFAULT_GAIN),
        rg_settings_get_number(NS_GLOBAL, SETTING_GGAIN, DEFAULT_GAIN),
        rg_settings_get_number(NS_GLOBAL, SETTING_BGAIN, DEFAULT_GAIN),
        rg_settings_get_number(NS_GLOBAL, SETTING_CONTRAST, DEFAULT_CONTRAST),
        rg_settings_get_number(NS_GLOBAL, SETTING_GAMMA, DEFAULT_GAMMA));
    if (display_preserve)
    {
        // Screen already holds the loading image; don't clear it. Consume the flag
        // and turn the backlight straight on so the image stays visible. The next
        // game's first frame overwrites the loading image when it's ready.
        rg_settings_set_number(NS_GLOBAL, "ScPreserve", 0);
        lcd_set_backlight(config.backlight);
    }
    else
    {
        rg_display_clear(C_BLACK);
        rg_task_delay(80); // Wait for the screen be cleared before turning on the backlight (40ms doesn't seem to be enough...)
        lcd_set_backlight(config.backlight);
    }
    display_task_queue = rg_task_create("rg_display", &display_task, NULL, 4 * 1024, RG_TASK_PRIORITY_6, 1);
#ifdef ESP_PLATFORM
    rg_task_create("rg_console", &serial_console_task, NULL, 3 * 1024, 2, 0);
#endif
    if (config.border_file)
        load_border_file(config.border_file);
    display_initialized = true;
    RG_LOGI("Display ready.\n");
}
