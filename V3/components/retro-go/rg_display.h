#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    RG_DISPLAY_SCALING_OFF = 0, // No scaling, center image on screen
    RG_DISPLAY_SCALING_FIT,     // Scale and preserve aspect ratio
    RG_DISPLAY_SCALING_FULL,    // Scale and stretch to fill screen
    RG_DISPLAY_SCALING_ZOOM,  // Custom zoom and preserve aspect ratio
    RG_DISPLAY_SCALING_COUNT
} display_scaling_t;

typedef enum
{
    RG_DISPLAY_FILTER_OFF = 0,
    RG_DISPLAY_FILTER_HORIZ,
    RG_DISPLAY_FILTER_VERT,
    RG_DISPLAY_FILTER_BOTH,
    RG_DISPLAY_FILTER_COUNT,
} display_filter_t;

typedef enum
{
    RG_DISPLAY_ROTATION_OFF = 0,
    RG_DISPLAY_ROTATION_AUTO,
    RG_DISPLAY_ROTATION_LEFT,
    RG_DISPLAY_ROTATION_RIGHT,
    RG_DISPLAY_ROTATION_COUNT,
} display_rotation_t;

typedef enum
{
    RG_DISPLAY_TYPE_ST7789  = 0, // V3 screen (HSD028B3N3-P 2.8" IPS)
    RG_DISPLAY_TYPE_ILI9341 = 1, // V2 screen
    RG_DISPLAY_TYPE_COUNT,
} display_type_t;

typedef enum
{
    RG_DISPLAY_BACKLIGHT_MIN = 1,
    RG_DISPLAY_BACKLIGHT_MAX = 100,
} display_backlight_t;

enum
{
    RG_DISPLAY_WRITE_NOSYNC = (1 << 0),
    RG_DISPLAY_WRITE_NOSWAP = (1 << 1),
};

typedef struct
{
    display_rotation_t rotation;
    display_scaling_t scaling;
    display_filter_t filter;
    display_backlight_t backlight;
    char *border_file;
    double custom_zoom;
    int osd_stats;      // 1 = fps/busy/heap status line in the screen border while playing
} rg_display_config_t;

typedef struct
{
    int32_t totalFrames;
    int32_t fullFrames;
    int32_t partFrames;
    int64_t blockTime;
    int64_t busyTime;
} rg_display_counters_t;

typedef struct
{
    const char *name;                                               // Driver name
    bool (*init)(void);                                             // Init the display
    bool (*deinit)(void);                                           // Deinit the display
    bool (*sync)(void);                                             // Pause until all data has been flushed
    bool (*set_backlight)(float percent);                           // Set backlight 0.0 - 1.0
    bool (*set_window)(int left, int top, int width, int height);   // Set draw window
    uint16_t *(*get_buffer)(size_t length);                         // Get a DMA-capable buffer to write pixels to and send via send_buffer
    bool (*send_buffer)(uint16_t *buffer, size_t length);           // Send data to display (buffer MUST be acquired via get_buffer)
    // bool (*write)(int left, int top, int width, int height, int pitch, const uint16_t data);
} rg_display_driver_t;

typedef struct
{
    struct
    {
        int real_width, real_height; // Real physical resolution
        int width, height; // Visible resolution (minus margins)
        struct {int left, top, right, bottom;} margins;
        int format;
    } screen;
    struct
    {
        int top, left;
        int width, height;
        float step_x, step_y;
        bool filter_x, filter_y;
    } viewport;
    struct
    {
        int width, height;
    } source;
    bool changed;
} rg_display_t;

#include "rg_surface.h"

void rg_display_init(void);
void rg_display_deinit(void);
void rg_display_write_rect(int left, int top, int width, int height, int stride, const uint16_t *buffer, uint32_t flags);
void rg_display_clear_rect(int left, int top, int width, int height, uint16_t color_le);
void rg_display_clear_except(int left, int top, int width, int height, uint16_t color_le);
void rg_display_clear(uint16_t color_le);
bool rg_display_sync(bool block);
void rg_display_force_redraw(void);
void rg_display_submit(const rg_surface_t *update, uint32_t flags);

rg_display_counters_t rg_display_get_counters(void);
const rg_display_t *rg_display_get_info(void);
int rg_display_get_width(void);
int rg_display_get_height(void);

void rg_display_set_type(display_type_t type);
display_type_t rg_display_get_type(void);
void rg_display_gscan_test(void);

void rg_display_set_scaling(display_scaling_t scaling);
display_scaling_t rg_display_get_scaling(void);
// Stats overlay (core/fps/busy/frameskip/heap) in the screen border while playing,
// refreshed once per second by the display task. Meant for Scaling=Off (free border);
// other scaling modes partially overwrite it each frame. Persisted (NS_APP "DispStats").
void rg_display_set_osd_stats(int enabled);
int  rg_display_get_osd_stats(void);
// Optionally implemented by the emulator (weak default = NULL): short label prefixed
// to the overlay, e.g. "jit"/"cla".
const char *rg_display_osd_tag(void);
void rg_display_set_filter(display_filter_t filter);
display_filter_t rg_display_get_filter(void);
void rg_display_set_rotation(display_rotation_t rotation);
display_rotation_t rg_display_get_rotation(void);
void rg_display_set_backlight(display_backlight_t percent);
display_backlight_t rg_display_get_backlight(void);
// On-screen low-battery icon. Off by default (the status LED already blinks for
// this); boards without that LED can turn it on.
void rg_display_set_battery_icon(bool enable);
bool rg_display_get_battery_icon(void);
// Turns the backlight off WITHOUT persisting it (unlike rg_display_set_backlight).
// Used to blank the screen during transitions so re-init/last-frame glitches stay hidden.
void rg_display_backlight_off(void);
// Stop accepting game frames (and finish the in-flight one). Called at shutdown so a
// late frame can't tear through / overwrite the loading screen during a transition.
void rg_display_block_submit(void);

// ST7789 VCOM voltage (0xBB register). Range 0..0x3F (~0.1V .. 1.875V, 0.025V/step).
// Higher VCOM = more saturated colors. Default 0x28 (1.1V, TFT_eSPI standard).
void rg_display_set_vcom(uint8_t value);
uint8_t rg_display_get_vcom(void);

// ST7789 VCOM Offset (0xC5 register). 8-bit fine-tune on top of VCOM.
// Primary knob for fighting vertical column crosstalk. Default 0x20.
void rg_display_set_vcom_offset(uint8_t value);
uint8_t rg_display_get_vcom_offset(void);

// ST7789 VRH (0xC3 register). Positive gamma voltage / column drive strength.
// Lower = less crosstalk, slightly less contrast. Range 0..0x3F, default 0x0F.
void rg_display_set_vrh(uint8_t value);
uint8_t rg_display_get_vrh(void);

// ST7789 frame rate (0xC6 FRCTR2/RTNA). 0x0F = 60Hz, 0x00 = 119Hz, lower value =
// higher Hz. Higher Hz reduces tearing but gives each line less charging time,
// which can cause horizontal crosstalk. Range 0..0x1F, default 0x02 (~105Hz).
void rg_display_set_framerate(uint8_t value);
uint8_t rg_display_get_framerate(void);

// Per-channel software color correction (LUT-based). Each gain is a percentage 25..200
// (50%=half intensity, 100%=unchanged, 150%=1.5x).
// 100/100/100 with contrast=100 and gamma=100 disables correction (zero CPU cost).
void rg_display_set_color_gains(int r_pct, int g_pct, int b_pct);
void rg_display_get_color_gains(int *r_pct, int *g_pct, int *b_pct);

// Software contrast around the midpoint (LUT-based). 50..150%.
void rg_display_set_contrast(int pct);
int rg_display_get_contrast(void);

// Software gamma curve, applied to all three channels (LUT-based). 50..200%.
void rg_display_set_gamma(int pct);
int rg_display_get_gamma(void);

// Visible-area margins in pixels — shrinks the rendered image inward on each side.
void rg_display_set_margins(int left, int top, int right, int bottom);
void rg_display_get_margins(int *left, int *top, int *right, int *bottom);

void rg_display_set_border(const char *filename);
char *rg_display_get_border(void);
void rg_display_set_custom_zoom(double factor);
double rg_display_get_custom_zoom(void);
