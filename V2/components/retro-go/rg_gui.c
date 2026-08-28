#include "rg_system.h"
#include "rg_gui.h"

#include <cJSON.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "bitmaps/image_loading.h"
#include "fonts/fonts.h"

// Username shown for the file server. HTTP Basic auth always renders a username
// field in the browser's login box, so give it a real value instead of leaving
// customers guessing what belongs there. webui.c ignores whatever is typed and
// only checks the password, so an empty username keeps working too.
#define RG_WEBUI_USER "blockboy"

// DMG palette -- RGB565 -- mirrors gnuboy GB_PALETTE_DMG (yellow-olive).
// Used by the tabbed dialog and as defaults for rg_gui_set_theme.
#define DMG_LIGHTEST  0x94C0
#define DMG_LIGHT     0x5383
#define DMG_DARK      0x2A83
#define DMG_DARKEST   0x1200

// Embedded theme JSON blobs — declared via COMPONENT_EMBED_TXTFILES in CMakeLists.
// EMBED_TXTFILES blobs are null-terminated, so they parse directly with cJSON.
extern const char dmg_json_start[]     asm("_binary_dmg_json_start");
extern const char dmg_json_end[]       asm("_binary_dmg_json_end");
extern const char classic_json_start[] asm("_binary_classic_json_start");
extern const char classic_json_end[]   asm("_binary_classic_json_end");

typedef struct {
    const char *name;
    const char *start;
    const char *end;
} embedded_theme_t;

static const embedded_theme_t embedded_themes[] = {
    { "dmg",     dmg_json_start,     dmg_json_end     },
    { "classic", classic_json_start, classic_json_end },
    { NULL, NULL, NULL }
};

static struct
{
    uint16_t *screen_buffer, *draw_buffer;
    size_t draw_buffer_size;
    int screen_width, screen_height;
    struct {int left, top, right, bottom;} margins;
    struct
    {
        const rg_font_t *font;
        int font_height;
        rg_color_t box_background;
        rg_color_t box_header;
        rg_color_t box_border;
        rg_color_t item_standard;
        rg_color_t item_disabled;
        rg_color_t item_message;
        rg_color_t scrollbar;
        rg_color_t shadow;
    } style;
    char theme_name[32];
    cJSON *theme_obj;
    int font_index;
    bool show_clock;
    bool initialized;
} gui;

#define SETTING_FONTTYPE    "FontType"
#define SETTING_CLOCK       "Clock"
#define SETTING_THEME       "Theme"
#define SETTING_WIFI_ENABLE "Enable"
#define SETTING_WIFI_SLOT   "Slot"
#define SETTING_LANGUAGE    "Language"

static uint16_t *get_draw_buffer(int width, int height, rg_color_t fill_color)
{
    size_t pixels = width * height;
    if (pixels > gui.draw_buffer_size)
    {
        if (gui.draw_buffer != NULL)
        {
            RG_LOGW("Growing drawing buffer to %dx%d...", width, height);
            free(gui.draw_buffer);
        }
        gui.draw_buffer = rg_alloc(pixels * 2, MEM_SLOW);
        gui.draw_buffer_size = pixels;
    }

    if (!gui.draw_buffer)
        RG_PANIC("Failed to allocate draw buffer!");

    if (fill_color != C_NONE)
    {
        for (size_t i = 0; i < pixels; ++i)
            gui.draw_buffer[i] = fill_color;
    }

    return gui.draw_buffer;
}

static int get_horizontal_position(int x_pos, int width)
{
    int type = (x_pos & 0xFF0000) | 0x8000;
    int offset = (x_pos & 0xFFFF) - 0x8000;
    if (type == RG_GUI_CENTER)
        return ((gui.screen_width - width) / 2) + offset;
    else if (type == RG_GUI_LEFT)
        return 0 + offset;
    else if (type == RG_GUI_RIGHT)
        return (gui.screen_width - width) - offset;
    else if (x_pos < 0)
        return x_pos + gui.screen_width;
    return x_pos;
}

static int get_vertical_position(int y_pos, int height)
{
    int type = (y_pos & 0xFF0000) | 0x8000;
    int offset = (y_pos & 0xFFFF) - 0x8000;
    if (type == RG_GUI_CENTER)
        return ((gui.screen_height - height) / 2) + offset;
    else if (type == RG_GUI_TOP)
        return 0 + offset;
    else if (type == RG_GUI_BOTTOM)
        return (gui.screen_height - height) - offset;
    else if (y_pos < 0)
        return y_pos + gui.screen_height;
    return y_pos;
}

void rg_gui_init(void)
{
    gui.screen_width = rg_display_get_width();
    gui.screen_height = rg_display_get_height();
    // FIXME: RG_SCREEN_SAFE_AREA being added on top of RG_SCREEN_VISIBLE_AREA might not be super intuitive
    //        because of how this is defined in config.h. It should be documented somewhere...
    gui.margins = (__typeof__(gui.margins))RG_SCREEN_SAFE_AREA;
    gui.draw_buffer = get_draw_buffer(gui.screen_width, 18, C_BLACK);
    rg_gui_set_language_id(rg_settings_get_number(NS_GLOBAL, SETTING_LANGUAGE, RG_LANG_EN));
    rg_gui_set_font(rg_settings_get_number(NS_GLOBAL, SETTING_FONTTYPE, RG_FONT_VERA_11));
    rg_gui_set_theme(rg_settings_get_string(NS_GLOBAL, SETTING_THEME, NULL));
    gui.show_clock = rg_settings_get_boolean(NS_GLOBAL, SETTING_CLOCK, false);
    gui.initialized = true;
}

bool rg_gui_set_language_id(int index)
{
    if (rg_localization_set_language_id(index))
    {
        rg_settings_set_number(NS_GLOBAL, SETTING_LANGUAGE, index);
        RG_LOGI("Language set to: %s (%d)", rg_localization_get_language_name(index), index);
        return true;
    }
    rg_localization_set_language_id(RG_LANG_EN);
    RG_LOGE("Invalid language id %d!", index);
    return false;
}

bool rg_gui_set_theme(const char *theme_name)
{
    char pathbuf[RG_PATH_MAX];
    cJSON *new_theme = NULL;

    // Cleanup the current theme
    cJSON_Delete(gui.theme_obj);
    gui.theme_obj = NULL;

    if (theme_name && theme_name[0])
    {
        // 1. Try embedded themes first (compiled into firmware, no SD needed)
        for (int i = 0; embedded_themes[i].name; i++)
        {
            if (strcmp(theme_name, embedded_themes[i].name) != 0)
                continue;
            new_theme = cJSON_Parse(embedded_themes[i].start);
            if (!new_theme) // Parse failure — try fixup on a writable copy
            {
                size_t n = embedded_themes[i].end - embedded_themes[i].start;
                char *tmp = malloc(n + 1);
                if (tmp) {
                    memcpy(tmp, embedded_themes[i].start, n);
                    tmp[n] = 0;
                    new_theme = cJSON_Parse(rg_json_fixup(tmp));
                    free(tmp);
                }
            }
            break;
        }

        // 2. Fallback: try SD-card path (so users can still drop in custom themes)
        if (!new_theme)
        {
            snprintf(pathbuf, RG_PATH_MAX, "%s/%s/theme.json", RG_BASE_PATH_THEMES, theme_name);
            char *data;
            size_t data_len;
            if (rg_storage_read_file(pathbuf, (void **)&data, &data_len, 0))
            {
                new_theme = cJSON_Parse(data);
                if (!new_theme) // Parse failure, clean the markup and try again
                    new_theme = cJSON_Parse(rg_json_fixup(data));
                free(data);
            }
            if (!new_theme)
                RG_LOGE("Failed to load theme '%s' (embedded + '%s')!\n", theme_name, pathbuf);
        }
    }

    if (new_theme)
    {
        rg_settings_set_string(NS_GLOBAL, SETTING_THEME, theme_name);
        strcpy(gui.theme_name, theme_name);
        // FIXME: Keeping the theme around uses quite a lot of internal memory (about 3KB)...
        //        We should probably convert it to a regular array or hashmap.
        gui.theme_obj = new_theme;
        RG_LOGI("Theme set to '%s'!\n", theme_name);
    }
    else
    {
        rg_settings_set_string(NS_GLOBAL, SETTING_THEME, NULL);
        strcpy(gui.theme_name, "");
        gui.theme_obj = NULL;
        RG_LOGI("Using built-in theme!\n");
    }

    gui.style.box_background = rg_gui_get_theme_color("dialog", "background", DMG_LIGHTEST);
    gui.style.box_header     = rg_gui_get_theme_color("dialog", "header",     DMG_DARKEST);
    gui.style.box_border     = rg_gui_get_theme_color("dialog", "border",     DMG_DARKEST);
    gui.style.item_standard  = rg_gui_get_theme_color("dialog", "item_standard", DMG_DARKEST);
    gui.style.item_disabled  = rg_gui_get_theme_color("dialog", "item_disabled", DMG_DARK);
    gui.style.item_message   = rg_gui_get_theme_color("dialog", "item_message",  DMG_DARK);
    gui.style.scrollbar      = rg_gui_get_theme_color("dialog", "scrollbar",     DMG_DARK);
    gui.style.shadow         = rg_gui_get_theme_color("dialog", "shadow",        C_NONE);

    return true;
}

rg_color_t rg_gui_get_theme_color(const char *section, const char *key, rg_color_t default_value)
{
    cJSON *root = section ? cJSON_GetObjectItem(gui.theme_obj, section) : gui.theme_obj;
    cJSON *obj = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(obj))
        return obj->valueint;
    char *strval = cJSON_GetStringValue(obj);
    if (!strval || strlen(strval) < 4)
        return default_value;
    if (strcmp(strval, "transparent") == 0)
        return C_TRANSPARENT;
    if (strcmp(strval, "none") == 0)
        return C_NONE;
    int intval = (int)strtol(strval, NULL, 0);
    // It is better to specify colors as RGB565 to avoid data loss, but we also accept RGB888 for convenience
    if (strlen(strval) == 8 && strval[0] == '0' && strval[1] == 'x')
        return (((intval >> 19) & 0x1F) << 11) | (((intval >> 10) & 0x3F) << 5) | (((intval >> 3) & 0x1F));
    return intval;
}

rg_image_t *rg_gui_get_theme_image(const char *name)
{
    char pathbuf[RG_PATH_MAX];
    if (!name || !gui.theme_name[0])
        return NULL;
    snprintf(pathbuf, RG_PATH_MAX, "%s/%s/%s", RG_BASE_PATH_THEMES, gui.theme_name, name);
    return rg_surface_load_image_file(pathbuf, 0);
}

const char *rg_gui_get_theme_name(void)
{
    return gui.theme_name[0] ? gui.theme_name : NULL;
}

bool rg_gui_set_font(int index)
{
    if (index < 0 || index > RG_FONT_MAX - 1)
        return false;

    const rg_font_t *font = fonts[index];

    gui.font_index = index;
    gui.style.font = font;
    gui.style.font_height = (index < 3) ? (8 + index * 4) : font->height;

    rg_settings_set_number(NS_GLOBAL, SETTING_FONTTYPE, index);

    RG_LOGI("Font set to: %s (height=%d, scaling=%.2f)\n",
        gui.style.font->name, gui.style.font_height, (float)gui.style.font_height / font->height);

    return true;
}

void rg_gui_set_surface(rg_surface_t *surface)
{
    gui.screen_buffer = surface ? surface->data : NULL;
}

static void copy_buffer_ex(int left, int top, int width, int height, int stride,
                           const void *buffer, uint32_t dflags)
{
    left = get_horizontal_position(left, width);
    top = get_vertical_position(top, height);

    if (gui.screen_buffer)
    {
        if (stride < width)
            stride = width * 2;

        width = RG_MIN(width, gui.screen_width - left);
        height = RG_MIN(height, gui.screen_height - top);

        for (int y = 0; y < height; ++y)
        {
            uint16_t *dst = gui.screen_buffer + (top + y) * gui.screen_width + left;
            const uint16_t *src = (void *)buffer + y * stride;
            for (int x = 0; x < width; ++x)
                if (src[x] != C_TRANSPARENT)
                    dst[x] = src[x];
        }
    }
    else
    {
        rg_display_write_rect(left, top, width, height, stride, buffer, dflags);
    }
}

void rg_gui_copy_buffer(int left, int top, int width, int height, int stride, const void *buffer)
{
    copy_buffer_ex(left, top, width, height, stride, buffer, 0);
}

static size_t get_glyph(uint32_t *output, const rg_font_t *font, int points, int c)
{
    // Some glyphs are always zero width
    if (!font || c == '\r' || c == '\n' || c == 0) // || c < 8 || c > 0xFFFF)
        return 0;

    if (points <= 0)
        points = font->height;

    const uint8_t *ptr = font->data;
    const rg_font_glyph_t *glyph = (rg_font_glyph_t *)ptr;
    // for (size_t i = 0; i < font->chars && glyph->code && glyph->code != c; ++i)
    while (glyph->code && glyph->code != c)
    {
        if (glyph->width != 0)
            ptr += (((glyph->width * glyph->height) - 1) / 8) + 1;
        ptr += sizeof(rg_font_glyph_t);
        glyph = (rg_font_glyph_t *)ptr;
    }

    if (glyph && glyph->code == c) // Glyph found
    {
        // Based on code by Boris Lovosevic (https://github.com/loboris)
        int yOffset = glyph->yOffset;
        int width = glyph->width;
        int height = glyph->height;
        int xOffset = glyph->xOffset < 0x80 ? glyph->xOffset : -(0xFF - glyph->xOffset);
        int xDelta = glyph->xDelta;
        const uint8_t *data = glyph->data;
        if (output)
        {
            memset(output, 0, points * 4);
            int ch = 0, mask = 0x80;
            for (int y = 0; y < height; y++)
            {
                uint32_t row = 0;
                for (int x = 0; x < width; x++)
                {
                    if (((x + (y * width)) % 8) == 0)
                    {
                        mask = 0x80;
                        ch = *data++;
                    }
                    if ((ch & mask) != 0)
                        row |= (1 << (xOffset + x));
                    mask >>= 1;
                }
                output[yOffset + y] = row;
            }
            // Vertical stretching
            if (points != font->height)
            {
                float scale = (float)points / font->height;
                for (int y = points - 1; y >= 0; y--)
                    output[y] = output[(int)(y / scale)];
            }
        }
        return RG_MAX(width, xDelta);
    }
    // else if (font != &font_basic8x8) // Glyph not found, try fallback font
    // {
    //     return get_glyph(output, &font_basic8x8, points, c);
    // }
    else // Glyph not found, no fallback
    {
        size_t box_width = font->width ?: 8;
        if (output) // draw missing box
        {
            uint32_t mask = ~((0xFFFFFFFF << (box_width - 1)) | 1);
            for (size_t i = 0; i < points; ++i)
                output[i] = (0xAAAAAAAA << (i & 1)) & mask;
        }
        return box_width;
    }
}

rg_rect_t rg_gui_draw_text(int x_pos, int y_pos, int width, const char *text, // const rg_font_t *font,
                           rg_color_t color_fg, rg_color_t color_bg, uint32_t flags)
{
    const rg_font_t *font = gui.style.font;
    int padding = (flags & RG_TEXT_NO_PADDING) ? 0 : 1;
    int font_height = (flags & RG_TEXT_BIGGER) ? gui.style.font_height * 2 : gui.style.font_height;
    int monospace = ((flags & RG_TEXT_MONOSPACE) || font->type == 0) ? font->width : 0;
    int line_height = font_height + padding * 2;
    int line_count = 0;
    // int16_t line_breaks[64], line_width_cache[64];

    if (!text || *text == 0)
        text = " ";

    if (width == 0)
    {
        // Find the longest line to determine our box width
        int line_width = padding * 2;
        for (const char *ptr = text; *ptr;)
        {
            int chr = rg_utf8_get_codepoint(&ptr);
            line_width += monospace ?: get_glyph(NULL, font, font_height, chr);

            if (chr == '\n' || *ptr == 0)
            {
                width = RG_MAX(line_width, width);
                line_width = padding * 2;
                line_count++;
            }
        }
    }

    x_pos = get_horizontal_position(x_pos, width);
    y_pos = get_vertical_position(y_pos, line_height);

    if (x_pos + width > gui.screen_width || y_pos + line_height > gui.screen_height)
    {
        RG_LOGD("Texbox (pos: %dx%d, size: %dx%d) will be truncated!", width, line_height, x_pos, y_pos);
        // return;
    }

    int draw_width = RG_MIN(width, gui.screen_width - x_pos);
    int y_offset = 0;

    for (const char *ptr = text; *ptr;)
    {
        const char *line_start_ptr = ptr;
        int x_offset = padding;

        if (flags & (RG_TEXT_ALIGN_RIGHT|RG_TEXT_ALIGN_CENTER))
        {
            // Find the current line's text width
            const char *line = ptr;
            while (x_offset < draw_width && *line && *line != '\n')
            {
                int chr = rg_utf8_get_codepoint(&line);
                int width = monospace ?: get_glyph(NULL, font, font_height, chr);
                if (draw_width - x_offset < width) // Do not truncate glyphs
                    break;
                x_offset += width;
            }
            if (flags & RG_TEXT_ALIGN_CENTER)
                x_offset = (draw_width - x_offset) / 2;
            else if (flags & RG_TEXT_ALIGN_RIGHT)
                x_offset = draw_width - x_offset;
        }

        uint16_t *draw_buffer = NULL;

        if (!(flags & RG_TEXT_DUMMY_DRAW))
            draw_buffer = get_draw_buffer(draw_width, line_height, color_bg);

        while (x_offset < draw_width)
        {
            uint32_t bitmap[font_height];
            const char *prev_ptr = ptr;
            int glyph_width = get_glyph(bitmap, font, font_height, rg_utf8_get_codepoint(&ptr));
            int width = monospace ?: glyph_width;

            if (draw_width - x_offset < width) // Do not truncate glyphs
            {
                // Wrap to the next line only if we've already placed a glyph on this
                // one. A single glyph wider than the whole line at the start would
                // rewind ptr forever here (infinite loop -> the "app unresponsive"
                // watchdog), so leave ptr advanced and skip it instead.
                if ((flags & RG_TEXT_MULTILINE) && x_offset > padding)
                    ptr = prev_ptr;
                break;
            }

            if (!(flags & RG_TEXT_DUMMY_DRAW))
            {
                for (int y = 0; y < font_height; y++)
                {
                    uint32_t row = bitmap[y];
                    if (row != 0) // get_draw_buffer fills the bg color, nothing to do if row empty
                    {
                        uint16_t *output = &draw_buffer[(draw_width * (y + padding)) + x_offset];
                        for (int x = 0; x < width; x++)
                            output[x] = ((row >> x) & 1) ? color_fg : color_bg;
                    }
                }
            }

            x_offset += width;

            if (*ptr == 0 || *ptr == '\n')
                break;
        }

        if (!(flags & RG_TEXT_DUMMY_DRAW))
            copy_buffer_ex(x_pos, y_pos + y_offset, draw_width, line_height, 0, draw_buffer,
                           (flags & RG_TEXT_NO_SYNC) ? RG_DISPLAY_WRITE_NOSYNC : 0);

        y_offset += line_height;

        // Safety net: if this line consumed no characters at all (e.g. draw_width
        // is smaller than a single glyph), stop instead of spinning forever.
        if (!(flags & RG_TEXT_MULTILINE) || ptr == line_start_ptr)
            break;
    }

    return (rg_rect_t){x_pos, y_pos, draw_width, y_offset};
}

void rg_gui_draw_rect(int x_pos, int y_pos, int width, int height, int border_size,
                      rg_color_t border_color, rg_color_t fill_color)
{
    if (width <= 0 || height <= 0)
        return;

    x_pos = get_horizontal_position(x_pos, width);
    y_pos = get_vertical_position(y_pos, height);

    if (border_size > 0)
    {
        uint16_t *draw_buffer = get_draw_buffer(border_size, RG_MAX(width, height), border_color);

        rg_gui_copy_buffer(x_pos, y_pos, width, border_size, 0, draw_buffer);                        // Top
        rg_gui_copy_buffer(x_pos, y_pos + height - border_size, width, border_size, 0, draw_buffer); // Bottom
        rg_gui_copy_buffer(x_pos, y_pos, border_size, height, 0, draw_buffer);                       // Left
        rg_gui_copy_buffer(x_pos + width - border_size, y_pos, border_size, height, 0, draw_buffer); // Right

        x_pos += border_size;
        y_pos += border_size;
        width -= border_size * 2;
        height -= border_size * 2;
    }

    if (width > 0 && height > 0 && fill_color != C_NONE)
    {
        uint16_t *draw_buffer = get_draw_buffer(width, RG_MIN(height, 16), fill_color);
        for (int y = 0; y < height; y += 16)
            rg_gui_copy_buffer(x_pos, y_pos + y, width, RG_MIN(height - y, 16), 0, draw_buffer);
    }
}

void rg_gui_draw_image(int x_pos, int y_pos, int width, int height, bool resample, const rg_image_t *img)
{
    if (img && resample && (width && height) && (width != img->width || height != img->height))
    {
        rg_image_t *new_img = rg_surface_resize(img, width, height);
        rg_gui_copy_buffer(x_pos, y_pos, width, height, new_img->width * 2, new_img->data);
        rg_surface_free(new_img);
    }
    else if (img)
    {
        int draw_width = width ? RG_MIN(width, img->width) : img->width;
        int draw_height = height ? RG_MIN(height, img->height) : img->height;
        rg_gui_copy_buffer(x_pos, y_pos, draw_width, draw_height, img->width * 2, img->data);
    }
    else // We fill a rect to show something is missing instead of abort...
    {
        rg_gui_draw_rect(x_pos, y_pos, width, height, 2, C_RED, C_BLACK);
        // rg_gui_draw_text(x_pos + 2, y_pos + 2, width - 4, "No image", C_DIM_GRAY, C_BLACK, 0);
    }
}

void rg_gui_draw_icons(void)
{
    rg_battery_t battery = rg_input_read_battery();
    rg_network_t network = rg_network_get_info();
    rg_rect_t txt = TEXT_RECT("00:00", 0);
    int bar_height = txt.height;
    int icon_height = RG_MAX(8, bar_height - 4);
    int icon_top = RG_MAX(0, (bar_height - icon_height - 1) / 2);
    int right = gui.margins.right;

    if (battery.present)
    {
        right += 22;

        int width = 16;
        int height = icon_height;
        int width_fill = width / 100.f * battery.level;
        int x_pos = -right;
        int y_pos = icon_top;

        rg_color_t color_fill = (battery.level > 20 ? (battery.level > 40 ? C_FOREST_GREEN : C_ORANGE) : C_RED);
        rg_color_t color_border = C_SILVER;
        rg_color_t color_empty = C_BLACK;

        rg_gui_draw_rect(x_pos, y_pos, width + 2, height, 1, color_border, C_NONE);
        rg_gui_draw_rect(x_pos + width + 2, y_pos + 2, 2, height - 4, 1, color_border, C_NONE);
        rg_gui_draw_rect(x_pos + 1, y_pos + 1, width_fill, height - 2, 0, 0, color_fill);
        rg_gui_draw_rect(x_pos + 1 + width_fill, y_pos + 1, width - width_fill, height - 2, 0, 0, color_empty);
    }

    if (network.state > RG_NETWORK_DISCONNECTED)
    {
        right += 22;

        int width = 16;
        int height = icon_height;
        int seg_width = (width - 2 - 2) / 3;
        int x_pos = -right;
        int y_pos = icon_top;

        rg_color_t color_fill = (network.state == RG_NETWORK_CONNECTED) ? C_GREEN : C_NONE;
        rg_color_t color_border = (network.state == RG_NETWORK_CONNECTED) ? C_SILVER : C_DIM_GRAY;

        y_pos += height * 0.6;
        rg_gui_draw_rect(x_pos, y_pos, seg_width, height * 0.4, 1, color_border, color_fill);
        x_pos += seg_width + 2;
        y_pos -= height * 0.3;
        rg_gui_draw_rect(x_pos, y_pos, seg_width, height * 0.7, 1, color_border, color_fill);
        x_pos += seg_width + 2;
        y_pos -= height * 0.3;
        rg_gui_draw_rect(x_pos, y_pos, seg_width, height * 1.0, 1, color_border, color_fill);
    }

    if (gui.show_clock)
    {
        right += txt.width + 4;

        int x_pos = -right;
        int y_pos = 0;
        char buffer[12];
        time_t time_sec = time(NULL);
        struct tm *time = localtime(&time_sec);

        int h = time->tm_hour, m = time->tm_min;
        if (rg_settings_get_number(NS_GLOBAL, "Clock24h", 1))
        {
            sprintf(buffer, "%02d:%02d", h % 100, m % 60);
        }
        else
        {
            int h12 = h % 12; if (h12 == 0) h12 = 12;
            sprintf(buffer, "%d:%02d%s", h12, m % 60, (h % 24 < 12) ? "AM" : "PM");
        }
        rg_gui_draw_text(x_pos, y_pos, 0, buffer, C_SILVER, gui.screen_buffer ? C_TRANSPARENT : C_BLACK, 0);
    }
}

void rg_gui_draw_loading(void)
{
    rg_display_clear(0x94C0);  // gnuboy GB_PALETTE_DMG lightest — matches boot animation and menus
    rg_display_write_rect(
        get_horizontal_position(RG_GUI_CENTER, image_loading.width),
        get_vertical_position(RG_GUI_CENTER, image_loading.height),
        image_loading.width,
        image_loading.height,
        image_loading.width * 2,
        (uint16_t*)image_loading.pixel_data, 0);
    rg_display_draw_led_dot();
}

static bool suppress_status_bars = false;

void rg_gui_draw_status_bars(void)
{
    if (suppress_status_bars)
        return;

    size_t max_len = gui.screen_width / 8;
    char header[max_len];
    char footer[max_len];

    const rg_app_t *app = rg_system_get_app();
    rg_stats_t stats = rg_system_get_counters();

    if (!app->initialized || app->isLauncher)
        return;

    snprintf(header, max_len, "SPEED: %d%% (%d %d) / BUSY: %d%%",
        (int)round(stats.totalFPS / app->tickRate * 100.f),
        (int)round(stats.totalFPS),
        (int)app->frameskip,
        (int)round(stats.busyPercent));

    if (app->romPath && strlen(app->romPath) > max_len - 1)
        snprintf(footer, max_len, "...%s", app->romPath + (strlen(app->romPath) - (max_len - 4)));
    else if (app->romPath)
        snprintf(footer, max_len, "%s", app->romPath);
    else
        snprintf(footer, max_len, "Retro-Go %s", app->version);

    // FIXME: Respect gui.margins (draw black background full screen_width, but pad the text if needed)
    rg_gui_draw_text(0, RG_GUI_TOP, gui.screen_width, header, C_WHITE, C_BLACK, 0);
    rg_gui_draw_text(0, RG_GUI_BOTTOM, gui.screen_width, footer, C_WHITE, C_BLACK, 0);

    rg_gui_draw_icons();
}

static size_t get_dialog_items_count(const rg_gui_option_t *options)
{
    if (!options)
        return 0;

    const rg_gui_option_t *opt = options;
    while (opt->arg || opt->label || opt->value || opt->flags || opt->update_cb)
        opt++;
    return opt - options;
}

void rg_gui_draw_dialog(const char *title, const rg_gui_option_t *options, int sel)
{
    const size_t options_count = get_dialog_items_count(options);
    const int sep_width = TEXT_RECT(": ", 0).width;
    const int font_height = gui.style.font_height;
    const int max_box_width = 0.82f * gui.screen_width;
    const int max_box_height = 0.74f * gui.screen_height;
    const int box_padding = 6;
    const int row_padding_y = 0; // now handled by draw_text
    const int row_padding_x = 8;
    const int max_inner_width = max_box_width - sep_width - (row_padding_x + box_padding) * 2;
    const int min_row_height = TEXT_RECT(" ", max_inner_width).height + row_padding_y * 2;

    int box_width = box_padding * 2;
    int box_height = box_padding * 2 + (title ? font_height + 6 : 0);
    int inner_width = TEXT_RECT(title, 0).width;
    int col1_width = -1;
    int col2_width = -1;
    // Heap-allocated (not a stack VLA) so dialogs with many rows — e.g. the
    // showcase "Select games" list with one entry per ROM — don't overflow the
    // task stack. Bail out silently on OOM rather than crashing.
    int *row_height = calloc(options_count, sizeof(int));
    if (!row_height)
        return;

    for (size_t i = 0; i < options_count; i++)
    {
        rg_rect_t label = {0, 0, 0, min_row_height};
        rg_rect_t value = {0};

        if (options[i].flags == RG_DIALOG_FLAG_HIDDEN)
        {
            row_height[i] = 0;
            continue;
        }

        if (options[i].label)
        {
            label = TEXT_RECT(options[i].label, max_inner_width);
            inner_width = RG_MAX(inner_width, label.width);
        }

        if (options[i].value)
        {
            value = TEXT_RECT(options[i].value, max_inner_width - label.width);
            col1_width = RG_MAX(col1_width, label.width);
            col2_width = RG_MAX(col2_width, value.width);
        }

        row_height[i] = RG_MAX(label.height, value.height) + row_padding_y * 2;
        box_height += row_height[i];
    }

    col1_width = RG_MIN(col1_width, max_box_width);
    col2_width = RG_MIN(col2_width, max_box_width);

    if (col2_width >= 0)
        inner_width = RG_MAX(inner_width, col1_width + col2_width + sep_width);

    inner_width = RG_MIN(inner_width, max_box_width);
    col2_width = inner_width - col1_width - sep_width;

    // Second pass. The measurements above used max_inner_width, but rows are
    // drawn at inner_width (values at col2_width), which is narrower as soon as
    // no row needs the full width. Text then wraps onto more lines than we
    // budgeted for and the box comes out too short. Measure again with exactly
    // the widths the draw loop below uses.
    box_height = box_padding * 2 + (title ? font_height + 6 : 0);
    for (size_t i = 0; i < options_count; i++)
    {
        rg_rect_t label = {0, 0, 0, min_row_height};
        rg_rect_t value = {0};

        if (options[i].flags == RG_DIALOG_FLAG_HIDDEN)
            continue; // row_height[i] is already 0

        if (options[i].value)
        {
            // A value row draws its label on one line, clipped to col1_width,
            // and only the value is allowed to wrap.
            if (options[i].label)
                label = (rg_rect_t){0, 0, col1_width, font_height};
            value = TEXT_RECT(options[i].value, col2_width);
        }
        else if (options[i].label)
        {
            label = TEXT_RECT(options[i].label, inner_width);
        }

        row_height[i] = RG_MAX(label.height, value.height) + row_padding_y * 2;
        box_height += row_height[i];
    }

    box_width += inner_width + row_padding_x * 2;
    box_height = RG_MIN(box_height, max_box_height);

    const int box_x = (gui.screen_width - box_width) / 2;
    const int box_y = (gui.screen_height - box_height) / 2;

    int x = box_x + box_padding;
    int y = box_y + box_padding;

    // Rows may only occupy the area inside the padding: the frame drawn after
    // the row loop paints box_padding pixels of background back over the edges.
    // Measuring against box_y + box_height instead let the bottom row spill into
    // that band and get half repainted -- always the last row, because it is the
    // only one without the 4px scroll-arrow slack to hide it.
    const int content_bottom = box_y + box_height - box_padding;

    if (title)
    {
        int width = inner_width + row_padding_x * 2;
        rg_gui_draw_text(x, y, width, title, gui.style.box_header, gui.style.box_background, RG_TEXT_ALIGN_CENTER);
        rg_gui_draw_rect(x, y + font_height, width, 6, 0, 0, gui.style.box_background);
        y += font_height + 6;
    }

    int top_i = 0;

    if (sel >= 0 && sel < options_count)
    {
        int yy = y;

        for (int i = 0; i < options_count; i++)
        {
            // Mirror the draw-loop's slack so page boundaries match exactly.
            int slack = ((size_t)i + 1 < options_count) ? 4 : 0;
            if (yy + row_height[i] + slack > content_bottom)
            {
                if (sel < i) break;
                yy = y;
                top_i = i;
            }
            yy += row_height[i];
        }
    }

    int i = top_i;
    for (; i < options_count; i++)
    {
        uint16_t color, fg, bg;
        int xx = x + row_padding_x;
        int yy = y + row_padding_y;
        int height = 8;

        if (options[i].flags == RG_DIALOG_FLAG_NORMAL)
            color = gui.style.item_standard;
        else if (options[i].flags == RG_DIALOG_FLAG_MESSAGE)
            color = gui.style.item_message;
        else
            color = gui.style.item_disabled;

        // Only NORMAL items get the selection highlight. DISABLED items get cursor focus
        // (so the dialog scrolls to show them) but render as plain grey text without the
        // inverted-color highlight — used for read-only multi-line content like the About box.
        bool highlight = options[i].flags == RG_DIALOG_FLAG_NORMAL && i == sel;
        fg = highlight ? gui.style.box_background : color;
        bg = highlight ? color : gui.style.box_background;

        // Leave 4px slack for the scroll-down arrow — but only if more rows follow.
        int slack = ((size_t)i + 1 < options_count) ? 4 : 0;
        if (y + row_height[i] + slack > content_bottom)
            break;

        if (options[i].flags == RG_DIALOG_FLAG_HIDDEN)
            continue;

        if (false && options[i].flags == RG_DIALOG_FLAG_SEPARATOR)
        {
            // FIXME: Draw a nice dim line...
        }
        else if (options[i].value)
        {
            rg_gui_draw_text(xx, yy, col1_width, options[i].label, fg, bg, 0);
            rg_gui_draw_text(xx + col1_width, yy, sep_width, ": ", fg, bg, 0);
            height = rg_gui_draw_text(xx + col1_width + sep_width, yy, col2_width, options[i].value, fg, bg, RG_TEXT_MULTILINE).height;
            if ((height / font_height) >= 2) // Multiline value, must fill sep and label
                rg_gui_draw_rect(xx, yy + font_height + 1, inner_width - col2_width, height - font_height, 0, 0, bg);
        }
        else
        {
            height = rg_gui_draw_text(xx, yy, inner_width, options[i].label, fg, bg, RG_TEXT_MULTILINE).height;
        }

        rg_gui_draw_rect(x, yy, row_padding_x, height, 0, 0, bg);
        rg_gui_draw_rect(xx + inner_width, yy, row_padding_x, height, 0, 0, bg);
        rg_gui_draw_rect(x, y, inner_width + row_padding_x * 2, row_padding_y, 0, 0, bg);
        rg_gui_draw_rect(x, yy + height, inner_width + row_padding_x * 2, row_padding_y, 0, 0, bg);

        y += height + row_padding_y * 2;
    }

    if (y < (box_y + box_height))
    {
        rg_gui_draw_rect(box_x, y, box_width, (box_y + box_height) - y, 0, 0, gui.style.box_background);
    }

    rg_gui_draw_rect(box_x, box_y, box_width, box_height, box_padding, gui.style.box_background, C_NONE);
    rg_gui_draw_rect(box_x - 1, box_y - 1, box_width + 2, box_height + 2, 1, gui.style.box_border, C_NONE);

    // Basic scroll indicators are overlayed at the end...
    if (top_i > 0)
    {
        int x = box_x + box_width - 10;
        int y = box_y + box_padding + 2;
        rg_gui_draw_rect(x + 0, y - 0, 6, 2, 0, 0, gui.style.scrollbar);
        rg_gui_draw_rect(x + 1, y - 2, 4, 2, 0, 0, gui.style.scrollbar);
        rg_gui_draw_rect(x + 2, y - 4, 2, 2, 0, 0, gui.style.scrollbar);
    }

    if (i < options_count)
    {
        int x = box_x + box_width - 10;
        int y = box_y + box_height - 6;
        rg_gui_draw_rect(x + 0, y - 4, 6, 2, 0, 0, gui.style.scrollbar);
        rg_gui_draw_rect(x + 1, y - 2, 4, 2, 0, 0, gui.style.scrollbar);
        rg_gui_draw_rect(x + 2, y - 0, 2, 2, 0, 0, gui.style.scrollbar);
    }

    free(row_height);

    rg_display_draw_led_dot(); // keep the fixed LED on top of every dialog/message
}

void rg_gui_draw_message(const char *format, ...)
{
    RG_ASSERT_ARG(format);

    char buffer[512];
    va_list va;
    va_start(va, format);
    vsnprintf(buffer, sizeof(buffer), format, va);
    va_end(va);
    const rg_gui_option_t options[] = {
        {0, buffer, NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        RG_DIALOG_END,
    };
    // FIXME: Should rg_display_force_redraw() be called? Before? After? Both?
    rg_gui_draw_dialog(NULL, options, 0);
}

intptr_t rg_gui_dialog(const char *title, const rg_gui_option_t *options_const, int selected_index)
{
    size_t options_count = get_dialog_items_count(options_const);
    int sel = selected_index < 0 ? (options_count + selected_index) : selected_index;
    int sel_old = -1;
    bool redraw = false;

    // Constrain initial cursor and skip FLAG_SKIP items
    sel = RG_MIN(RG_MAX(0, sel), options_count - 1);

    // We create a copy of options because the callbacks might modify it (ie option->value).
    // PSRAM-allocated (not a stack VLA, and MEM_SLOW to bypass the 32KB internal-RAM
    // threshold) so dialogs with many items — e.g. the showcase "Select games" list
    // with one entry per ROM — neither overflow the task stack nor exhaust internal RAM.
    rg_gui_option_t *options = rg_alloc((options_count + 1) * sizeof(rg_gui_option_t), MEM_SLOW);
    char *text_buffer = rg_alloc(options_count * 32, MEM_SLOW);
    char *text_buffer_ptr = text_buffer;

    if (!options)
    {
        free(text_buffer);
        return RG_DIALOG_CANCELLED;
    }
    memcpy(options, options_const, (options_count + 1) * sizeof(rg_gui_option_t));

    for (size_t i = 0; i < options_count; i++)
    {
        rg_gui_option_t *option = &options[i];
        if (!option->label)
            option->label = "";
        if (option->value && text_buffer)
            option->value = strcpy(text_buffer_ptr, option->value);
        if (option->update_cb)
            option->update_cb(option, RG_DIALOG_INIT);
        if (option->value && text_buffer)
            text_buffer_ptr += RG_MAX(strlen(option->value), 31) + 1;
    }

    rg_gui_draw_status_bars();
    rg_gui_draw_dialog(title, options, sel);
    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
    rg_task_delay(80);

    rg_gui_event_t event = RG_DIALOG_VOID;
    uint32_t joystick = 0, joystick_old;
    uint64_t joystick_last = 0;

    while (event != RG_DIALOG_SELECT && event != RG_DIALOG_CANCEL)
    {
        // TO DO: Add acceleration!
        joystick_old = ((rg_system_timer() - joystick_last) > 300000) ? 0 : joystick;
        joystick = rg_input_read_gamepad();
        event = RG_DIALOG_VOID;

        if (joystick ^ joystick_old)
        {
            bool active_selection = options[sel].flags == RG_DIALOG_FLAG_NORMAL;
            rg_gui_callback_t callback = active_selection ? options[sel].update_cb : NULL;

            if (joystick & RG_KEY_UP) {
                if (--sel < 0) sel = options_count - 1;
            }
            else if (joystick & RG_KEY_DOWN) {
                if (++sel > options_count - 1) sel = 0;
            }
            else if (joystick & (RG_KEY_B|RG_KEY_OPTION|RG_KEY_MENU)) {
                event = RG_DIALOG_CANCEL;
            }
            else if (joystick & RG_KEY_LEFT && callback) {
                event = callback(&options[sel], RG_DIALOG_PREV);
                redraw = true;
            }
            else if (joystick & RG_KEY_RIGHT && callback) {
                event = callback(&options[sel], RG_DIALOG_NEXT);
                redraw = true;
            }
            else if (joystick & RG_KEY_A && callback) {
                event = callback(&options[sel], RG_DIALOG_ENTER);
                redraw = true;
            }
            else if (joystick & RG_KEY_A && active_selection) {
                event = RG_DIALOG_SELECT;
            }

            joystick_last = rg_system_timer();
        }

        if (sel_old != sel)
        {
            for (size_t i = 0; i < options_count; ++i)
            {
                // If the item is selectable, we stop here
                if (options[sel].flags == RG_DIALOG_FLAG_NORMAL)
                    break;
                if (options[sel].flags == RG_DIALOG_FLAG_DISABLED)
                    break;

                // Otherwise move to the next
                sel += (joystick == RG_KEY_UP) ? -1 : 1;

                if (sel < 0)
                    sel = options_count - 1;

                if (sel >= options_count)
                    sel = 0;
            }
            if (sel_old != -1 && options[sel_old].update_cb)
                options[sel_old].update_cb(&options[sel_old], RG_DIALOG_FOCUS_LOST);
            if (options[sel].update_cb)
                options[sel].update_cb(&options[sel], RG_DIALOG_FOCUS_GAINED);
            redraw = true;
            sel_old = sel;
        }

        if (event == RG_DIALOG_REDRAW || event == RG_DIALOG_UPDATE)
        {
            for (size_t i = 0; i < options_count; i++)
            {
                if (options[i].update_cb)
                    options[i].update_cb(&options[i], RG_DIALOG_UPDATE);
            }
            if (event == RG_DIALOG_REDRAW)
            {
                rg_display_force_redraw();
                rg_gui_draw_status_bars();
            }
            redraw = true;
        }

        if (redraw)
        {
            rg_gui_draw_dialog(title, options, sel);
            redraw = false;
        }

        rg_task_delay(20);
        rg_system_tick(0);
    }

    rg_input_wait_for_key(joystick, false, 1000);
    rg_display_force_redraw();
    free(text_buffer);

    intptr_t result = (event == RG_DIALOG_CANCEL || sel < 0) ? RG_DIALOG_CANCELLED : options[sel].arg;
    free(options);
    return result;
}

bool rg_gui_confirm(const char *title, const char *message, bool default_yes)
{
    const rg_gui_option_t options[] = {
        {0, message, NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, "",      NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {1, _("Yes"),   NULL, RG_DIALOG_FLAG_NORMAL,  NULL},
        {0, _("No"),   NULL, RG_DIALOG_FLAG_NORMAL,  NULL},
        RG_DIALOG_END,
    };
    return rg_gui_dialog(title, message ? options : options + 1, default_yes ? -2 : -1) == 1;
}

void rg_gui_alert(const char *title, const char *message)
{
    const rg_gui_option_t options[] = {
        {0, message, NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, "",      NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {1, _("OK"),    NULL, RG_DIALOG_FLAG_NORMAL,  NULL},
        RG_DIALOG_END,
    };
    rg_gui_dialog(title, message ? options : options + 1, -1);
}

typedef struct
{
    rg_gui_option_t options[22];
    size_t count;
    bool (*validator)(const char *path);
} file_picker_opts_t;

static int file_picker_cb(const rg_scandir_t *entry, void *arg)
{
    file_picker_opts_t *f = arg;
    if (f->validator && !(f->validator)(entry->path))
        return RG_SCANDIR_SKIP;
    char *path = strdup(entry->path);
    f->options[f->count].arg = (intptr_t)path;
    f->options[f->count].flags = RG_DIALOG_FLAG_NORMAL;
    f->options[f->count].label = rg_basename(path);
    f->count++;
    if (f->count > 18)
        return RG_SCANDIR_STOP;
    return RG_SCANDIR_CONTINUE;
}

char *rg_gui_file_picker(const char *title, const char *path, bool (*validator)(const char *path), bool none_option)
{
    file_picker_opts_t options = {
        .options = {},
        .count = 0,
        .validator = validator,
    };

    if (!title)
        title = _("Select file");

    if (none_option)
    {
        options.options[options.count++] = (rg_gui_option_t){0, _("<None>"), NULL, RG_DIALOG_FLAG_NORMAL, NULL};
        // options.options[options.count++] = (rg_gui_option_t)RG_DIALOG_SEPARATOR;
    }

    if (!rg_storage_scandir(path, file_picker_cb, &options, 0) || options.count < 1)
    {
        rg_gui_alert(title, _("Folder is empty."));
        return NULL;
    }

    options.options[options.count] = (rg_gui_option_t)RG_DIALOG_END;

    char *filepath = (char *)rg_gui_dialog(title, options.options, 0);

    if (filepath != (void *)RG_DIALOG_CANCELLED)
        filepath = strdup(filepath ? filepath : "");
    else
        filepath = NULL;

    for (size_t i = 0; i < options.count; ++i)
        free((void *)(options.options[i].arg));

    return filepath;
}

// ===== Virtual keyboard (ported from retro-go 1.46) =====
// Three switchable layouts: lower-case, upper-case, symbols. SELECT cycles them.
static const rg_keyboard_layout_t keyboard_layouts[] = {
    {
        .layout = "1234567890" "qwertyuiop" "asdfghjkl " "zxcvbnm.,?",
        .columns = 10, .rows = 4, .label = "ABC",
    },
    {
        .layout = "1234567890" "QWERTYUIOP" "ASDFGHJKL " "ZXCVBNM.,?",
        .columns = 10, .rows = 4, .label = "abc",
    },
    {
        .layout = "!@#$%^&*()" "[]{}|\\:;\"'" "<>?/+=_-~ " "1234567890",
        .columns = 10, .rows = 4, .label = "!@#",
    },
};

void rg_gui_draw_virtual_keyboard(int x_pos, int y_pos, const rg_keyboard_layout_t *current_layout, int cursor_pos, bool partial_redraw)
{
    const int key_width = gui.screen_width / 10 - 4;
    const int key_height = 20;
    const int keyboard_width = current_layout->columns * key_width;
    const int keyboard_height = current_layout->rows * key_height;
    const int keyboard_x = get_horizontal_position(x_pos, keyboard_width);
    const int keyboard_y = get_vertical_position(y_pos, keyboard_height);
    const char *layout_ptr = current_layout->layout;

    if (!partial_redraw)
        rg_gui_draw_rect(keyboard_x - 2, keyboard_y - 2, keyboard_width + 4, keyboard_height + 4, 2,
            gui.style.box_border, gui.style.box_background);

    for (int row = 0; row < (int)current_layout->rows; row++)
    {
        for (int col = 0; col < (int)current_layout->columns; col++)
        {
            int key_idx = row * current_layout->columns + col;
            int x = keyboard_x + col * key_width;
            int y = keyboard_y + row * key_height;
            bool is_selected = (cursor_pos == key_idx);
            rg_color_t bg_color = is_selected ? gui.style.item_standard : gui.style.box_background;
            rg_color_t fg_color = is_selected ? gui.style.box_background : gui.style.item_standard;
            rg_color_t border_color = is_selected ? gui.style.item_standard : gui.style.box_border;

            rg_gui_draw_rect(x + 1, y + 1, key_width - 2, key_height - 2, 1, border_color, bg_color);

            char key_str[5] = {0};
            int key = rg_utf8_decode(&layout_ptr);
            if (key == ' ')
                strcpy(key_str, "SP");
            else
                rg_utf8_encode(key_str, key);
            rg_gui_draw_text(x + 2, y + 2, key_width - 4, key_str, fg_color, bg_color, RG_TEXT_ALIGN_CENTER);
        }
    }
}

void rg_gui_draw_input_screen(const char *title, const char *message, const char *input_buffer,
                              const rg_keyboard_layout_t *current_layout, int cursor_pos, bool partial_redraw)
{
    const int key_width = gui.screen_width / 10 - 4;
    const int key_height = 20;
    const int keyboard_width = current_layout->columns * key_width;
    const int keyboard_height = current_layout->rows * key_height;
    const int keyboard_x = (gui.screen_width - keyboard_width) / 2;
    const int keyboard_y = gui.screen_height - keyboard_height - 40;
    const int input_box_height = 30;
    const int input_box_y = keyboard_y - input_box_height - 10;
    char text_buffer[200];

    if (!input_buffer)
        input_buffer = "";

    if (!partial_redraw)
    {
        rg_gui_draw_rect(0, 0, gui.screen_width, gui.screen_height, 0, C_NONE, gui.style.box_background);
        if (title)
            rg_gui_draw_text(0, 10, gui.screen_width, title, gui.style.box_header, gui.style.box_background, RG_TEXT_ALIGN_CENTER);
        if (message)
            rg_gui_draw_text(0, title ? 35 : 10, gui.screen_width, message, gui.style.item_message, gui.style.box_background, RG_TEXT_ALIGN_CENTER);
        rg_gui_draw_rect(keyboard_x, input_box_y, keyboard_width, input_box_height, 2, gui.style.box_border, C_WHITE);
        snprintf(text_buffer, sizeof(text_buffer), "A=Type B=Bksp SELECT=%3s START=OK MENU=Cancel", current_layout->label);
        rg_gui_draw_text(0, gui.screen_height - 15, gui.screen_width, text_buffer, gui.style.item_message, gui.style.box_background, RG_TEXT_ALIGN_CENTER);
    }

    snprintf(text_buffer, sizeof(text_buffer), "%s_", input_buffer);
    rg_gui_draw_text(keyboard_x + 5, input_box_y + 5, keyboard_width - 10, text_buffer, C_BLACK, C_WHITE, 0);
    rg_gui_draw_virtual_keyboard(keyboard_x, keyboard_y, current_layout, cursor_pos, true);
}

char *rg_gui_input_str(const char *title, const char *message, const char *default_value)
{
    char input_buffer[128] = {0};
    if (default_value)
        strncpy(input_buffer, default_value, sizeof(input_buffer) - 1);

    int cursor_pos = 0, layout_idx = 0;
    int input_length = strlen(input_buffer);
    bool cancelled = false;
    const rg_keyboard_layout_t *current_layout = &keyboard_layouts[layout_idx];

    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
    rg_task_delay(80);

    uint32_t joystick = 0, joystick_old;
    uint64_t joystick_last = 0;
    bool redraw = true;
    int redraws = 0;

    while (true)
    {
        joystick_old = ((rg_system_timer() - joystick_last) > 300000) ? 0 : joystick;
        joystick = rg_input_read_gamepad();

        if (joystick ^ joystick_old)
        {
            int keys = current_layout->columns * current_layout->rows;
            if (joystick & RG_KEY_LEFT)       { if (--cursor_pos < 0) cursor_pos = keys - 1; redraw = true; }
            else if (joystick & RG_KEY_RIGHT) { if (++cursor_pos >= keys) cursor_pos = 0; redraw = true; }
            else if (joystick & RG_KEY_UP)    { cursor_pos -= current_layout->columns; if (cursor_pos < 0) cursor_pos += keys; redraw = true; }
            else if (joystick & RG_KEY_DOWN)  { cursor_pos += current_layout->columns; if (cursor_pos >= keys) cursor_pos -= keys; redraw = true; }
            else if (joystick & RG_KEY_A)
            {
                if (input_length < (int)sizeof(input_buffer) - 4)
                {
                    const char *layout_ptr = current_layout->layout;
                    int key = 0;
                    for (int i = 0; i <= cursor_pos; ++i)
                        key = rg_utf8_decode(&layout_ptr);
                    input_length += rg_utf8_encode(&input_buffer[input_length], key);
                    input_buffer[input_length] = '\0';
                    redraw = true;
                }
            }
            else if (joystick & RG_KEY_B)
            {
                while (input_length > 0)
                {
                    const char *ptr = &input_buffer[--input_length];
                    if (rg_utf8_decode(&ptr) != -1)
                        break;
                }
                input_buffer[input_length] = '\0';
                redraw = true;
            }
            else if (joystick & RG_KEY_SELECT)
            {
                layout_idx = (layout_idx + 1) % RG_COUNT(keyboard_layouts);
                current_layout = &keyboard_layouts[layout_idx];
                cursor_pos = 0;
                redraw = true;
                redraws = 0;
            }
            else if (joystick & RG_KEY_START)
                break;
            else if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
            {
                cancelled = true;
                break;
            }
            joystick_last = rg_system_timer();
        }

        if (redraw)
        {
            rg_gui_draw_input_screen(title, message, input_buffer, current_layout, cursor_pos, redraws++ > 0);
            redraw = false;
        }

        rg_task_delay(20);
        rg_system_tick(0);
    }

    rg_input_wait_for_key(joystick, false, 1000);
    rg_display_force_redraw();

    if (cancelled)
        return NULL;
    return input_length > 0 ? strdup(input_buffer) : NULL;
}

void rg_gui_draw_keyboard(const rg_keyboard_map_t *map, size_t cursor)
{
    RG_ASSERT_ARG(map);

    int width = map->columns * 16 + 16;
    int height = map->rows * 16 + 16;

    int x_pos = (gui.screen_width - width) / 2;
    int y_pos = (gui.screen_height - height);

    char buf[2] = {0};

    rg_gui_draw_rect(x_pos, y_pos, width, height, 2, gui.style.box_border, gui.style.box_background);

    for (size_t i = 0; i < map->columns * map->rows; ++i)
    {
        int x = x_pos + 8 + (i % map->columns) * 16;
        int y = y_pos + 8 + (i / map->columns) * 16;
        if (!map->data[i])
            continue;
        buf[0] = map->data[i];
        rg_gui_draw_text(x + 1, y + 1, 14, buf, C_BLACK, i == cursor ? C_CYAN : C_IVORY, RG_TEXT_ALIGN_CENTER);
    }
}

static rg_gui_event_t volume_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int level = rg_audio_get_volume();
    int prev_level = level;

    if (event == RG_DIALOG_PREV)
        level -= 5;
    if (event == RG_DIALOG_NEXT)
        level += 5;

    level -= (level % 5);

    if (level != prev_level)
        rg_audio_set_volume(level);

    sprintf(option->value, "%d%%", rg_audio_get_volume());

    return RG_DIALOG_VOID;
}

static rg_gui_event_t brightness_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int level = rg_display_get_backlight();
    int prev_level = level;

    if (event == RG_DIALOG_PREV)
        level -= 10;
    if (event == RG_DIALOG_NEXT)
        level += 10;

    level -= (level % 10);

    if (level != prev_level)
        rg_display_set_backlight(RG_MAX(level, 1));

    sprintf(option->value, "%d%%", rg_display_get_backlight());

    return RG_DIALOG_VOID;
}

static rg_gui_event_t vcom_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int v = rg_display_get_vcom();
    if (event == RG_DIALOG_PREV && v > 0)    v--;
    if (event == RG_DIALOG_NEXT && v < 0x3F) v++;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_display_set_vcom((uint8_t)v);
    sprintf(option->value, "0x%02X", v);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t color_gain_update_cb(rg_gui_option_t *opt, rg_gui_event_t event, int channel)
{
    int r, g, b;
    rg_display_get_color_gains(&r, &g, &b);
    int *target = (channel == 0) ? &r : (channel == 1) ? &g : &b;

    if (event == RG_DIALOG_PREV) *target -= 5;
    if (event == RG_DIALOG_NEXT) *target += 5;

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_display_set_color_gains(r, g, b);

    rg_display_get_color_gains(&r, &g, &b);
    int shown = (channel == 0) ? r : (channel == 1) ? g : b;
    sprintf(opt->value, "%d%%", shown);
    return RG_DIALOG_VOID;
}
static rg_gui_event_t color_r_cb(rg_gui_option_t *o, rg_gui_event_t e) { return color_gain_update_cb(o, e, 0); }
static rg_gui_event_t color_g_cb(rg_gui_option_t *o, rg_gui_event_t e) { return color_gain_update_cb(o, e, 1); }
static rg_gui_event_t color_b_cb(rg_gui_option_t *o, rg_gui_event_t e) { return color_gain_update_cb(o, e, 2); }

static rg_gui_event_t contrast_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int c = rg_display_get_contrast();
    if (event == RG_DIALOG_PREV) c -= 5;
    if (event == RG_DIALOG_NEXT) c += 5;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_display_set_contrast(c);
    sprintf(option->value, "%d%%", rg_display_get_contrast());
    return RG_DIALOG_VOID;
}

static rg_gui_event_t gamma_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int g = rg_display_get_gamma();
    if (event == RG_DIALOG_PREV) g -= 5;
    if (event == RG_DIALOG_NEXT) g += 5;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_display_set_gamma(g);
    sprintf(option->value, "%d%%", rg_display_get_gamma());
    return RG_DIALOG_VOID;
}

static rg_gui_event_t color_order_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    bool bgr = rg_display_get_color_order();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_display_set_color_order(bgr = !bgr);
    strcpy(option->value, bgr ? _("BGR") : _("RGB"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t color_invert_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    bool inv = rg_display_get_inversion();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_display_set_inversion(inv = !inv);
    strcpy(option->value, inv ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t flip_180_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    bool flip = rg_display_get_flip_180();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_display_set_flip_180(flip = !flip);
    strcpy(option->value, flip ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t color_reset_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER) {
        rg_display_set_vcom(0x28);
        rg_display_set_color_gains(100, 100, 100);
        rg_display_set_contrast(100);
        rg_display_set_gamma(100);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t color_menu_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER) {
        const rg_gui_option_t color_options[] = {
            {0, _("Color order"), "-", RG_DIALOG_FLAG_NORMAL, &color_order_cb},
            {0, _("Invert"),      "-", RG_DIALOG_FLAG_NORMAL, &color_invert_cb},
            {0, _("VCOM"),     "-", RG_DIALOG_FLAG_NORMAL, &vcom_update_cb},
            {0, _("Contrast"), "-", RG_DIALOG_FLAG_NORMAL, &contrast_update_cb},
            {0, _("Gamma"),    "-", RG_DIALOG_FLAG_NORMAL, &gamma_update_cb},
            {0, _("Red"),      "-", RG_DIALOG_FLAG_NORMAL, &color_r_cb},
            {0, _("Green"),    "-", RG_DIALOG_FLAG_NORMAL, &color_g_cb},
            {0, _("Blue"),     "-", RG_DIALOG_FLAG_NORMAL, &color_b_cb},
            {0, _("Reset"),    NULL, RG_DIALOG_FLAG_NORMAL, &color_reset_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(_("Color settings"), color_options, 0);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

// Direct per-side margin control. Side: 0=L, 1=R, 2=T, 3=B.
static rg_gui_event_t margin_side_cb(rg_gui_option_t *option, rg_gui_event_t event, int side)
{
    int l, t, r, b;
    rg_display_get_margins(&l, &t, &r, &b);
    int *val = (side == 0) ? &l : (side == 1) ? &r : (side == 2) ? &t : &b;
    if (event == RG_DIALOG_PREV && *val >= 2) (*val) -= 2;
    else if (event == RG_DIALOG_PREV && *val > 0) *val = 0;
    if (event == RG_DIALOG_NEXT) (*val) += 2;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT) {
        rg_display_set_margins(l, t, r, b);
        rg_display_get_margins(&l, &t, &r, &b);
        sprintf(option->value, "%d px", *val);
        return RG_DIALOG_REDRAW;
    }
    sprintf(option->value, "%d px", *val);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t margin_l_cb(rg_gui_option_t *o, rg_gui_event_t e) { return margin_side_cb(o, e, 0); }
static rg_gui_event_t margin_r_cb(rg_gui_option_t *o, rg_gui_event_t e) { return margin_side_cb(o, e, 1); }
static rg_gui_event_t margin_t_cb(rg_gui_option_t *o, rg_gui_event_t e) { return margin_side_cb(o, e, 2); }
static rg_gui_event_t margin_b_cb(rg_gui_option_t *o, rg_gui_event_t e) { return margin_side_cb(o, e, 3); }

static rg_gui_event_t margin_reset_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER) {
        // Reset to the panel's tuned default (RG_SCREEN_VISIBLE_AREA) so the
        // lens-correct alignment is restored, not a full-screen 0,0,0,0.
        struct { int left, top, right, bottom; } def = RG_SCREEN_VISIBLE_AREA;
        rg_display_set_margins(def.left, def.top, def.right, def.bottom);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t screen_position_menu_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER) {
        const rg_gui_option_t pos_options[] = {
            {0, _("Rotate 180"), "-", RG_DIALOG_FLAG_NORMAL, &flip_180_cb},
            {0, _("Left"),   "-", RG_DIALOG_FLAG_NORMAL, &margin_l_cb},
            {0, _("Right"),  "-", RG_DIALOG_FLAG_NORMAL, &margin_r_cb},
            {0, _("Top"),    "-", RG_DIALOG_FLAG_NORMAL, &margin_t_cb},
            {0, _("Bottom"), "-", RG_DIALOG_FLAG_NORMAL, &margin_b_cb},
            {0, _("Reset"),    NULL, RG_DIALOG_FLAG_NORMAL, &margin_reset_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(_("Screen position"), pos_options, 0);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t audio_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    size_t count = 0;
    const rg_audio_sink_t *sinks = rg_audio_get_sinks(&count);
    const rg_audio_sink_t *ssink = rg_audio_get_sink();
    // Hide dummy unless it's the only one or we're a debug build
    int min = rg_system_get_app()->isRelease ? (1 % count) : (0);
    int max = count - 1;
    int sink = 0;

    // If there's no choice to be made we can just hide the entry
    if (min == max)
    {
        option->flags |= RG_DIALOG_FLAG_HIDDEN;
        return RG_DIALOG_VOID;
    }

    for (int i = 0; i < count; ++i)
        if (sinks[i].driver == ssink->driver && sinks[i].device == ssink->device)
            sink = i;

    int prev_sink = sink;

    if (event == RG_DIALOG_PREV && --sink < min)
        sink = max;
    if (event == RG_DIALOG_NEXT && ++sink > max)
        sink = min;

    if (sink != prev_sink)
        rg_audio_set_sink(sinks[sink].driver->name, sinks[sink].device);

    strcpy(option->value, sinks[sink].name);

    return RG_DIALOG_VOID;
}

static rg_gui_event_t filter_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int max = RG_DISPLAY_FILTER_COUNT - 1;
    int mode = rg_display_get_filter();
    int prev_mode = mode;

    if (event == RG_DIALOG_PREV && --mode < 0)
        mode = max;
    if (event == RG_DIALOG_NEXT && ++mode > max)
        mode = 0;

    if (mode != prev_mode)
    {
        rg_display_set_filter(mode);
        return RG_DIALOG_REDRAW;
    }

    if (mode == RG_DISPLAY_FILTER_OFF)
        strcpy(option->value, _("Off"));
    if (mode == RG_DISPLAY_FILTER_HORIZ)
        strcpy(option->value, _("Horiz"));
    if (mode == RG_DISPLAY_FILTER_VERT)
        strcpy(option->value, _("Vert"));
    if (mode == RG_DISPLAY_FILTER_BOTH)
        strcpy(option->value, _("Both"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t osd_stats_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_display_set_osd_stats(!rg_display_get_osd_stats());
    strcpy(option->value, rg_display_get_osd_stats() ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t scaling_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int max = RG_DISPLAY_SCALING_COUNT - 1;
    int mode = rg_display_get_scaling();
    int prev_mode = mode;

    if (event == RG_DIALOG_PREV && --mode < 0)
        mode = max; // 0;
    if (event == RG_DIALOG_NEXT && ++mode > max)
        mode = 0; // max;

    if (mode != prev_mode)
    {
        rg_display_set_scaling(mode);
        return RG_DIALOG_REDRAW;
    }

    if (mode == RG_DISPLAY_SCALING_OFF)
        strcpy(option->value, _("Off"));
    else if (mode == RG_DISPLAY_SCALING_FIT)
        strcpy(option->value, _("Fit"));
    else if (mode == RG_DISPLAY_SCALING_FULL)
        strcpy(option->value, _("Full"));
    else if (mode == RG_DISPLAY_SCALING_ZOOM)
        strcpy(option->value, _("Zoom"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t custom_zoom_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (rg_display_get_scaling() != RG_DISPLAY_SCALING_ZOOM)
    {
        option->flags = RG_DIALOG_FLAG_HIDDEN;
        return RG_DIALOG_VOID;
    }

    if (event == RG_DIALOG_PREV)
        rg_display_set_custom_zoom(rg_display_get_custom_zoom() - 0.05);
    if (event == RG_DIALOG_NEXT)
        rg_display_set_custom_zoom(rg_display_get_custom_zoom() + 0.05);

    sprintf(option->value, "%.2f", rg_display_get_custom_zoom());
    option->flags = RG_DIALOG_FLAG_NORMAL;

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        return RG_DIALOG_REDRAW;
    return RG_DIALOG_VOID;
}

static rg_gui_event_t overclock_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV)
        rg_system_set_overclock(rg_system_get_overclock() - 1);
    else if (event == RG_DIALOG_NEXT)
        rg_system_set_overclock(rg_system_get_overclock() + 1);
    sprintf(option->value, "%dMhz", 240 + (rg_system_get_overclock() * 40));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t speedup_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        float change = (event == RG_DIALOG_NEXT) ? 0.5f : -0.5f;
        rg_emu_set_speed(rg_emu_get_speed() + change);
    }
    sprintf(option->value, "%.1fx", rg_emu_get_speed());
    return RG_DIALOG_VOID;
}

static rg_gui_event_t led_indicator_opt_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        rg_system_set_indicator_mask(option->arg, !rg_system_get_indicator_mask(option->arg));
    }
    strcpy(option->value, rg_system_get_indicator_mask(option->arg) ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t led_indicator_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_gui_option_t options[] = {
            {RG_INDICATOR_ACTIVITY_SYSTEM, _("System activity"), "-", RG_DIALOG_FLAG_NORMAL, &led_indicator_opt_cb},
            {RG_INDICATOR_ACTIVITY_DISK, _("Disk activity"), "-", RG_DIALOG_FLAG_NORMAL, &led_indicator_opt_cb},
            {RG_INDICATOR_POWER_LOW, _("Low battery"), "-", RG_DIALOG_FLAG_NORMAL, &led_indicator_opt_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(option->label, options, 0);
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t show_clock_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        gui.show_clock = !gui.show_clock;
        rg_settings_set_boolean(NS_GLOBAL, SETTING_CLOCK, gui.show_clock);
        return RG_DIALOG_REDRAW;
    }
    strcpy(option->value, gui.show_clock ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t timezone_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char utc_offsets[][10] = {"UTC-12:00", "UTC-11:00", "UTC-10:00", "UTC-09:00", "UTC-09:30", "UTC-08:00",
                                    "UTC-07:00", "UTC-06:00", "UTC-05:00", "UTC-04:00", "UTC-03:30", "UTC-03:00",
                                    "UTC-02:00", "UTC-01:00", "UTC+00:00", "UTC+01:00", "UTC+02:00", "UTC+03:00",
                                    "UTC+03:30", "UTC+04:00", "UTC+04:30", "UTC+05:00", "UTC+05:30", "UTC+06:00",
                                    "UTC+06:30", "UTC+07:00", "UTC+08:00", "UTC+09:00", "UTC+09:30", "UTC+10:00",
                                    "UTC+10:30", "UTC+11:00", "UTC+12:00", "UTC+13:00", "UTC+14:00"};
    int index = 14; // UTC+00:00
    char *TZ = rg_system_get_timezone();
    if (TZ && strncmp(TZ, "UTC", 3) == 0)
    {
        // TZ has inverted offset for whatever reason
        TZ[3] = TZ[3] == '-' ? '+' : '-';
        for (size_t i = 0; i < RG_COUNT(utc_offsets); ++i)
        {
            if (strcmp(TZ, utc_offsets[i]) == 0)
            {
                index = i;
                break;
            }
        }
    }
    free(TZ);

    if (event == RG_DIALOG_ENTER)
    {
        // Popup list: pick the zone directly instead of cycling through ~35.
        rg_gui_option_t opts[RG_COUNT(utc_offsets) + 1];
        for (size_t i = 0; i < RG_COUNT(utc_offsets); ++i)
            opts[i] = (rg_gui_option_t){(intptr_t)i, utc_offsets[i], NULL, RG_DIALOG_FLAG_NORMAL, NULL};
        opts[RG_COUNT(utc_offsets)] = (rg_gui_option_t)RG_DIALOG_END;
        int sel = rg_gui_dialog(_("Timezone"), opts, index);
        if (sel >= 0 && sel < (int)RG_COUNT(utc_offsets))
        {
            char *newTZ = strdup(utc_offsets[sel]);
            newTZ[3] = newTZ[3] == '-' ? '+' : '-';
            rg_system_set_timezone(newTZ);
            free(newTZ);
            index = sel;
        }
        strcpy(option->value, utc_offsets[index]);
        return RG_DIALOG_REDRAW;
    }

    strcpy(option->value, utc_offsets[index]);
    return RG_DIALOG_VOID;
}

// ---- Clock submenu (Show clock / Set clock / Time format / Timezone) -------
static int rg_clock_is_24h(void) { return rg_settings_get_number(NS_GLOBAL, "Clock24h", 1); }

static rg_gui_event_t clock_format_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int h24 = rg_clock_is_24h();
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
    {
        h24 = !h24;
        rg_settings_set_number(NS_GLOBAL, "Clock24h", h24);
    }
    strcpy(option->value, h24 ? "24h" : "12h");
    return RG_DIALOG_VOID;
}

// Set-clock editor. Hours/minutes are edited on a held broken-down time and
// shown DIRECTLY from it — no settimeofday/localtime/mktime round-trip per
// keypress. The old per-press round-trip made the hour jump erratically
// (e.g. 23 -> 08 -> 03) whenever the system time or timezone was off; editing
// plain integers can only ever step by +/-1 with clean 0..23 / 0..59 wrap.
// The result is committed to the system clock once, when the dialog closes.
static struct tm clock_edit_tm;

static rg_gui_event_t clock_set_field_cb(rg_gui_option_t *option, rg_gui_event_t event, bool hours)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        int d = (event == RG_DIALOG_NEXT) ? 1 : -1;
        if (hours)
            clock_edit_tm.tm_hour = (clock_edit_tm.tm_hour + d + 24) % 24;
        else
            clock_edit_tm.tm_min = (clock_edit_tm.tm_min + d + 60) % 60;
    }
    if (hours && !rg_clock_is_24h())
    {
        int h12 = clock_edit_tm.tm_hour % 12; if (h12 == 0) h12 = 12;
        sprintf(option->value, "%d%s", h12, (clock_edit_tm.tm_hour < 12) ? "AM" : "PM");
    }
    else
        sprintf(option->value, "%02d", hours ? clock_edit_tm.tm_hour : clock_edit_tm.tm_min);
    return RG_DIALOG_VOID;
}
static rg_gui_event_t clock_set_h_cb(rg_gui_option_t *o, rg_gui_event_t e) { return clock_set_field_cb(o, e, true); }
static rg_gui_event_t clock_set_m_cb(rg_gui_option_t *o, rg_gui_event_t e) { return clock_set_field_cb(o, e, false); }

static rg_gui_event_t set_clock_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;
    time_t now = time(NULL);
    clock_edit_tm = *localtime(&now); // seed date + current H:M, then edit in place
    clock_edit_tm.tm_sec = 0;
    char h_v[16], m_v[16];
    clock_set_h_cb(&(rg_gui_option_t){0, NULL, h_v, 0, NULL}, RG_DIALOG_VOID);
    clock_set_m_cb(&(rg_gui_option_t){0, NULL, m_v, 0, NULL}, RG_DIALOG_VOID);
    const rg_gui_option_t opts[] = {
        {0, _("Hour"),   h_v, RG_DIALOG_FLAG_NORMAL, &clock_set_h_cb},
        {0, _("Minute"), m_v, RG_DIALOG_FLAG_NORMAL, &clock_set_m_cb},
        RG_DIALOG_END,
    };
    rg_gui_dialog(_("Set clock"), opts, 0);
    // Commit once: convert the edited local time to a timestamp and persist.
    clock_edit_tm.tm_sec = 0;
    clock_edit_tm.tm_isdst = -1;
    time_t t = mktime(&clock_edit_tm);
    if (t > 0)
    {
        settimeofday(&(struct timeval){t, 0}, NULL);
        rg_system_save_time(); // persist once, on exit
    }
    rg_settings_commit();
    return RG_DIALOG_REDRAW;
}

static rg_gui_event_t clock_menu_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;
    char show_v[16], tz_v[16], fmt_v[16];
    show_clock_cb(&(rg_gui_option_t){0, NULL, show_v, 0, NULL}, RG_DIALOG_VOID);
    timezone_cb(&(rg_gui_option_t){0, NULL, tz_v, 0, NULL}, RG_DIALOG_VOID);
    clock_format_cb(&(rg_gui_option_t){0, NULL, fmt_v, 0, NULL}, RG_DIALOG_VOID);
    const rg_gui_option_t opts[] = {
        {0, _("Show clock"),  show_v, RG_DIALOG_FLAG_NORMAL, &show_clock_cb},
        {0, _("Set clock"),   NULL,   RG_DIALOG_FLAG_NORMAL, &set_clock_cb},
        {0, _("Time format"), fmt_v,  RG_DIALOG_FLAG_NORMAL, &clock_format_cb},
        {0, _("Timezone"),    tz_v,   RG_DIALOG_FLAG_NORMAL, &timezone_cb},
        RG_DIALOG_END,
    };
    rg_gui_dialog(_("Clock"), opts, 0);
    rg_settings_commit();
    return RG_DIALOG_VOID;
}

static rg_gui_event_t font_type_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV && rg_gui_set_font(gui.font_index - 1))
        return RG_DIALOG_REDRAW;
    if (event == RG_DIALOG_NEXT && rg_gui_set_font(gui.font_index + 1))
        return RG_DIALOG_REDRAW;
    if (gui.style.font_height != gui.style.font->height)
        sprintf(option->value, "%s (%d)", gui.style.font->name, gui.style.font_height);
    else
        sprintf(option->value, "%s", gui.style.font->name);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t theme_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    rg_gui_event_t result = RG_DIALOG_VOID;

    // The themes table is embedded, so finding the current one is cheap enough
    // to do on every event instead of only when the picker opens.
    int count = 0;
    while (embedded_themes[count].name) count++;

    int sel = 0;
    const char *current = rg_gui_get_theme_name();
    if (current)
        for (int i = 0; i < count; i++)
            if (strcmp(current, embedded_themes[i].name) == 0) sel = i;

    if (count > 0 && (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT))
    {
        // Left/right steps through the list and wraps around, so you can try
        // themes straight from the menu. Enter still opens the full picker.
        sel += (event == RG_DIALOG_NEXT) ? 1 : -1;
        if (sel < 0)
            sel = count - 1;
        else if (sel >= count)
            sel = 0;
        rg_gui_set_theme(embedded_themes[sel].name);
        result = RG_DIALOG_REDRAW;
    }
    else if (count > 0 && event == RG_DIALOG_ENTER)
    {
        rg_gui_option_t opts[count + 1];
        for (int i = 0; i < count; i++)
            opts[i] = (rg_gui_option_t){i, embedded_themes[i].name, NULL, RG_DIALOG_FLAG_NORMAL, NULL};
        opts[count] = (rg_gui_option_t)RG_DIALOG_END;

        intptr_t chosen = rg_gui_dialog(_("Theme"), opts, sel);
        if (chosen >= 0 && chosen < count)
        {
            rg_gui_set_theme(embedded_themes[chosen].name);
            result = RG_DIALOG_REDRAW;
        }
    }

    // Always fall through — otherwise the old value sticks until the menu reopens.
    strcpy(option->value, rg_gui_get_theme_name() ?: embedded_themes[0].name);
    return result;
}

static rg_gui_event_t language_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int language_id = rg_localization_get_language_id();

    if (event == RG_DIALOG_ENTER)
    {
        rg_gui_option_t options[RG_LANG_MAX + 1];
        for (int i = 0; i < RG_LANG_MAX; i++)
            options[i] = (rg_gui_option_t){i, rg_localization_get_language_name(i), NULL, RG_DIALOG_FLAG_NORMAL, NULL};
        options[RG_LANG_MAX] = (rg_gui_option_t)RG_DIALOG_END;

        int sel = rg_gui_dialog(option->label, options, language_id);
        if (sel != RG_DIALOG_CANCELLED)
        {
            rg_gui_set_language_id(sel);
            if (rg_gui_confirm(_("Language changed!"), _("For these changes to take effect you must restart your device.\nrestart now?"), true))
            {
                rg_system_exit();
            }
            language_id = sel;
        }
        return RG_DIALOG_REDRAW;
    }

    sprintf(option->value, "%s", rg_localization_get_language_name(language_id) ?: "???");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t border_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        char *path = rg_gui_file_picker("Border", RG_BASE_PATH_BORDERS, NULL, true);
        if (path != NULL)
        {
            rg_display_set_border(strlen(path) ? path : NULL);
            free(path);
            return RG_DIALOG_REDRAW;
        }
    }
    char *border = rg_display_get_border();
    sprintf(option->value, "%.9s", border ? rg_basename(border) : _("None"));
    free(border);
    return RG_DIALOG_VOID;
}

#ifdef RG_ENABLE_NETWORKING
static void wifi_toggle_interactive(bool enable, int slot)
{
    rg_network_state_t target_state = enable ? RG_NETWORK_CONNECTED : RG_NETWORK_DISCONNECTED;
    int64_t timeout = rg_system_timer() + 20 * 1000000;
    rg_gui_draw_message(enable ? _("Connecting...") : _("Disconnecting..."));
    rg_network_wifi_stop();
    if (enable)
    {
        rg_wifi_config_t config = {0};
        rg_network_wifi_read_config(slot, &config);
        rg_network_wifi_set_config(&config);
        if (slot == 9000)
        {
            const rg_wifi_config_t config = {
                .ssid = "BlockBoy",
                .password = "blockboy",
                .channel = 6,
                .ap_mode = true,
            };
            rg_network_wifi_set_config(&config);
        }
        if (!rg_network_wifi_start())
            return;
    }
    do // Always loop at least once, in case we're in a transition
    {
        rg_task_delay(100);
        if (rg_system_timer() > timeout)
            break;
        if (rg_input_read_gamepad())
            break;
    } while (rg_network_get_info().state != target_state);
}

static rg_gui_event_t wifi_status_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    rg_network_t info = rg_network_get_info();
    // 0x12/0x13 are the file server credentials: those are worth showing even
    // when Wi-Fi is down, so handle them before the not-connected shortcut.
    if (option->arg == 0x15)
        strcpy(option->value, "blockboy.local");
    else if (option->arg == 0x14)
        strcpy(option->value, rg_gui_file_server_running() ? _("Running") : _("Off"));
    else if (option->arg == 0x12)
        strcpy(option->value, RG_WEBUI_USER);
    else if (option->arg == 0x13)
    {
        const char *pass = rg_gui_file_server_password();
        snprintf(option->value, 32, "%s", pass ? pass : _("(none)"));
    }
    else if (info.state != RG_NETWORK_CONNECTED)
        strcpy(option->value, _("Not connected"));
    else if (option->arg == 0x10)
        strcpy(option->value, info.name);
    else if (option->arg == 0x11)
        strcpy(option->value, info.ip_addr);
    return RG_DIALOG_VOID;
}

static rg_gui_event_t wifi_manage_slot_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int slot = option->arg;
    rg_wifi_config_t config = {0};

    if (event == RG_DIALOG_INIT || event == RG_DIALOG_UPDATE || event == RG_DIALOG_ENTER)
    {
        rg_network_wifi_read_config(slot, &config);
        strcpy(option->value, config.ssid[0] ? config.ssid : _("(add network)"));
    }

    if (event == RG_DIALOG_ENTER)
    {
        if (!config.ssid[0])
        {
            char *ssid = rg_gui_input_str(_("Wi-Fi SSID"), _("Enter network name:"), "");
            if (!ssid || strlen(ssid) == 0) { free(ssid); return RG_DIALOG_VOID; }
            char *password = rg_gui_input_str(_("Wi-Fi Password"), _("Enter password (empty = open):"), "");
            if (!password) password = strdup("");
            rg_wifi_config_t new_config = {0};
            strncpy(new_config.ssid, ssid, sizeof(new_config.ssid) - 1);
            strncpy(new_config.password, password, sizeof(new_config.password) - 1);
            free(ssid); free(password);
            if (!rg_network_wifi_write_config(slot, &new_config))
            {
                rg_gui_alert(_("Error"), _("Failed to save network"));
                return RG_DIALOG_VOID;
            }
            rg_settings_commit();
            config = new_config;
        }

        char title[50];
        snprintf(title, sizeof(title), "Slot %d: %.15s", slot, config.ssid);
        const rg_gui_option_t slot_options[] = {
            {1, _("Connect"),       NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {2, _("Edit SSID"),     NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {3, _("Edit Password"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {4, _("Delete"),        NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            RG_DIALOG_END,
        };
        int action = rg_gui_dialog(title, slot_options, 0);
        switch (action)
        {
            case 1:
                rg_settings_set_boolean(NS_WIFI, SETTING_WIFI_ENABLE, true);
                rg_settings_set_number(NS_WIFI, SETTING_WIFI_SLOT, slot);
                wifi_toggle_interactive(true, slot);
                break;
            case 2: {
                char *s = rg_gui_input_str(_("Edit SSID"), _("Network name:"), config.ssid);
                if (s && strlen(s) > 0) {
                    strncpy(config.ssid, s, sizeof(config.ssid) - 1); config.ssid[sizeof(config.ssid)-1]=0;
                    rg_network_wifi_write_config(slot, &config); rg_settings_commit();
                }
                free(s);
                break;
            }
            case 3: {
                char *p = rg_gui_input_str(_("Edit Password"), _("Password:"), config.password);
                if (p) {
                    strncpy(config.password, p, sizeof(config.password) - 1); config.password[sizeof(config.password)-1]=0;
                    rg_network_wifi_write_config(slot, &config); rg_settings_commit();
                }
                free(p);
                break;
            }
            case 4:
                if (rg_gui_confirm(_("Delete"), _("Delete this network?"), false)) {
                    rg_network_wifi_delete_config(slot); rg_settings_commit();
                }
                break;
        }
        strcpy(option->value, config.ssid[0] ? config.ssid : _("(empty)"));
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t wifi_manage_networks_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        rg_gui_option_t slot_options[] = {
            {0, _("Slot 0"), "-", RG_DIALOG_FLAG_NORMAL, &wifi_manage_slot_cb},
            {1, _("Slot 1"), "-", RG_DIALOG_FLAG_NORMAL, &wifi_manage_slot_cb},
            {2, _("Slot 2"), "-", RG_DIALOG_FLAG_NORMAL, &wifi_manage_slot_cb},
            {3, _("Slot 3"), "-", RG_DIALOG_FLAG_NORMAL, &wifi_manage_slot_cb},
            {4, _("Slot 4"), "-", RG_DIALOG_FLAG_NORMAL, &wifi_manage_slot_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(_("Manage networks"), slot_options, 0);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

// Lowest free Wi-Fi slot (0..4), or -1 if all used.
static int wifi_find_free_slot(void)
{
    rg_wifi_config_t probe;
    for (int i = 0; i < 5; i++)
        if (!rg_network_wifi_read_config(i, &probe))
            return i;
    return -1;
}

static rg_gui_event_t wifi_scan_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    rg_gui_draw_message(_("Scanning..."));
    rg_wifi_scan_result_t results[20];
    int count = rg_network_wifi_scan(results, RG_COUNT(results));

    if (count < 0) { rg_gui_alert(_("Error"), _("Scan failed.\nIs Wi-Fi enabled?")); return RG_DIALOG_REDRAW; }
    if (count == 0) { rg_gui_alert(_("Scan networks"), _("No networks found.")); return RG_DIALOG_REDRAW; }

    rg_gui_option_t scan_options[count + 1];
    char value_buffers[count][16];
    for (int i = 0; i < count; i++)
    {
        const char *strength = results[i].rssi >= -55 ? "****"
                             : results[i].rssi >= -65 ? "*** "
                             : results[i].rssi >= -75 ? "**  " : "*   ";
        const char *lock = results[i].auth_mode == 0 ? " " : "#";
        snprintf(value_buffers[i], sizeof(value_buffers[i]), "%s %s", strength, lock);
        scan_options[i] = (rg_gui_option_t){i, results[i].ssid, value_buffers[i], RG_DIALOG_FLAG_NORMAL, NULL};
    }
    scan_options[count] = (rg_gui_option_t)RG_DIALOG_END;

    intptr_t sel = rg_gui_dialog(_("Choose network"), scan_options, 0);
    if (sel < 0 || sel >= count)
        return RG_DIALOG_REDRAW;

    const rg_wifi_scan_result_t *chosen = &results[sel];
    char *password = NULL;
    if (chosen->auth_mode != 0)
    {
        password = rg_gui_input_str(_("Wi-Fi Password"), chosen->ssid, "");
        if (!password)
            return RG_DIALOG_REDRAW;
    }
    else
        password = strdup("");

    int slot = wifi_find_free_slot();
    if (slot < 0)
    {
        if (!rg_gui_confirm(_("All slots full"), _("Overwrite slot 0?"), false)) { free(password); return RG_DIALOG_REDRAW; }
        slot = 0;
    }

    rg_wifi_config_t new_config = {0};
    strncpy(new_config.ssid, chosen->ssid, sizeof(new_config.ssid) - 1);
    strncpy(new_config.password, password, sizeof(new_config.password) - 1);
    free(password);

    if (!rg_network_wifi_write_config(slot, &new_config))
    {
        rg_gui_alert(_("Error"), _("Failed to save network"));
        return RG_DIALOG_REDRAW;
    }
    rg_settings_set_boolean(NS_WIFI, SETTING_WIFI_ENABLE, true);
    rg_settings_set_number(NS_WIFI, SETTING_WIFI_SLOT, slot);
    rg_settings_commit();
    wifi_toggle_interactive(true, slot);
    return RG_DIALOG_REDRAW;
}

// Hosts the device's own Wi-Fi network so a phone or laptop can reach the file
// server without a router -- and without the customer having to type their WPA2
// password on the d-pad keyboard first. Hidden by default (vis_key "WifiAP");
// enable it from the Shop menu for customers who need it.
// NOTE: while the AP runs the device is off the home network, so SNTP time sync
// stops. That matters most on V1/V2, which have no battery-backed RTC.
static rg_gui_event_t wifi_access_point_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        if (rg_gui_confirm(_("Wi-Fi AP"), _("Start access point?\n\nSSID: BlockBoy\nPassword: blockboy\n\nBrowse: http://192.168.4.1/"), true))
        {
            wifi_toggle_interactive(true, 9000);
        }
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

// Hooks implemented by the launcher. The weak defaults are what the emulators
// get -- they have no file server, so the dialog below tells the user to go back
// to the main menu instead of showing a URL that can't work.
__attribute__((weak)) bool rg_gui_file_server_start(void)
{
    return false;
}

__attribute__((weak)) const char *rg_gui_file_server_password(void)
{
    return NULL;
}

__attribute__((weak)) bool rg_gui_file_server_running(void)
{
    return false;
}

__attribute__((weak)) bool rg_gui_file_server_set_password(void)
{
    return false;
}

__attribute__((weak)) bool rg_gui_file_server_set_enabled(bool enable)
{
    (void)enable;
    return false;
}

// Customer-facing help: explains that the device's own IP (the one shown at
// "IP address") is the address to open in a web browser to manage the SD card.
// Opening this dialog in the launcher also STARTS the file server for this
// session -- without that the promised URL never answered (the server defaults
// to off because it exposes the whole SD card).
// Uses single-line MESSAGE rows (not one multi-line string) so every line is
// measured and drawn; the URL lives in a static buffer so its pointer stays
// valid while the dialog is open.
static rg_gui_event_t wifi_info_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    rg_network_t info = rg_network_get_info();
    static char url_line[64], pass_line[64];
    bool connected = info.state == RG_NETWORK_CONNECTED && info.ip_addr[0];
    bool serving = connected && rg_gui_file_server_start();

    if (connected)
        snprintf(url_line, sizeof(url_line), "http://%s/", info.ip_addr);
    else
        snprintf(url_line, sizeof(url_line), "%s", _("Connect Wi-Fi first"));

    if (connected && !serving)
    {
        // Emulator app: the file server only runs in the launcher.
        const rg_gui_option_t opts[] = {
            {0, _("The file server runs in"),   NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
            {0, _("the main menu. Exit the"),   NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
            {0, _("game, then open:"),          NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
            {0, "",                             NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
            {0, "http://blockboy.local/",       NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
            {0, _("or, if that fails:"),        NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
            {0, url_line,                       NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
            RG_DIALOG_END,
        };
        rg_gui_dialog(_("SD card in browser"), opts, -1);
        return RG_DIALOG_REDRAW;
    }

    // The browser asks for the password on the very next screen, so show it here.
    const char *pass = serving ? rg_gui_file_server_password() : NULL;
    snprintf(pass_line, sizeof(pass_line), "%s %s", _("Password:"), pass ? pass : _("(none)"));

    // The name is easier to type and survives a changed IP, so it comes first.
    // mDNS is not everywhere though (older Android in particular), hence the
    // numeric address right below it as a fallback.
    const rg_gui_option_t opts[] = {
        {0, _("Open this in a browser"),    NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, _("to manage the SD card."),    NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, "",                             NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, connected ? "http://blockboy.local/" : "", NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, connected ? _("or, if that fails:") : "",  NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, url_line,                       NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        {0, serving ? pass_line : "",       NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
        RG_DIALOG_END,
    };
    rg_gui_dialog(_("SD card in browser"), opts, -1);
    return RG_DIALOG_REDRAW;
}

// Wi-Fi mode: Off / On / Sync only. "Sync only" connects briefly at boot to
// fetch the time via SNTP, then turns Wi-Fi off again (low battery).
static rg_gui_event_t wifi_mode_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int mode = rg_system_get_wifi_mode();
    bool changed = false;

    // Only Off/On are selectable ("Sync only" is disabled — see rg_system_get_wifi_mode)
    if (event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER) { mode = (mode + 1) % 2; changed = true; }
    else if (event == RG_DIALOG_PREV)                        { mode = (mode + 1) % 2; changed = true; }

    if (changed)
    {
        rg_system_set_wifi_mode(mode);
        rg_settings_commit();
        if (mode == RG_WIFI_MODE_ON)
        {
            wifi_toggle_interactive(true, rg_settings_get_number(NS_WIFI, SETTING_WIFI_SLOT, -1));
            // The file server should just be there whenever Wi-Fi is up, instead
            // of only after visiting Wi-Fi > Info. No-op in the emulators.
            rg_gui_file_server_start();
        }
        else if (mode == RG_WIFI_MODE_OFF)
            wifi_toggle_interactive(false, 0);
        else // RG_WIFI_MODE_SYNC_ONLY
        {
            rg_gui_draw_message(_("Syncing time..."));
            rg_system_sync_time_now();
        }
    }

    strcpy(option->value, mode == RG_WIFI_MODE_ON        ? _("On") :
                          mode == RG_WIFI_MODE_SYNC_ONLY ? _("Sync only") : _("Off"));
    return changed ? RG_DIALOG_REDRAW : RG_DIALOG_VOID;
}

// Het serverwachtwoord hoort thuis waar de rest van de servergegevens staat,
// en daar wil je hem ook meteen kunnen wijzigen in plaats van alleen aflezen.
// De invoer zelf zit in de launcher; hier vragen we er via het haakje om.
static rg_gui_event_t wifi_server_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    rg_gui_event_t result = RG_DIALOG_VOID;
    if (event == RG_DIALOG_ENTER || event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        if (rg_gui_file_server_set_enabled(!rg_gui_file_server_running()))
            result = RG_DIALOG_REDRAW;
    }
    strcpy(option->value, rg_gui_file_server_running() ? _("Running") : _("Off"));
    return result;
}

static rg_gui_event_t wifi_server_pass_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    rg_gui_event_t result = RG_DIALOG_VOID;
    if (event == RG_DIALOG_ENTER && rg_gui_file_server_set_password())
        result = RG_DIALOG_REDRAW;
    const char *pass = rg_gui_file_server_password();
    snprintf(option->value, 32, "%s", (pass && pass[0]) ? pass : _("(none)"));
    return result;
}

static rg_gui_event_t wifi_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        // Only the launcher implements the password hook; in an emulator there
        // is no file server, so hide the credential rows there.
        // Only the launcher implements the file server hooks; in an emulator
        // there is none, so those rows disappear entirely there.
        // The grey block behaves as a single row. MESSAGE rows are skipped
        // while navigating and DISABLED rows are not, even though they look
        // identical. Marking only the bottom grey row DISABLED makes one press
        // jump past the whole block and scroll the box in one go, instead of
        // four presses with no visible cursor. Without an anchor, everything
        // below the last selectable row would be unreachable.
        bool has_server = rg_gui_file_server_password() != NULL;
        int info_flag = has_server ? RG_DIALOG_FLAG_MESSAGE : RG_DIALOG_FLAG_HIDDEN;
        int edit_flag = has_server ? RG_DIALOG_FLAG_NORMAL : RG_DIALOG_FLAG_HIDDEN;
        // The anchor is always the BOTTOM VISIBLE grey row. Without a file
        // server "Web name" and "Server user" disappear, so the anchor moves
        // down to "IP address".
        int anchor_flag = RG_DIALOG_FLAG_DISABLED;
        int webname_flag = has_server ? anchor_flag : RG_DIALOG_FLAG_HIDDEN;
        int ip_flag = has_server ? RG_DIALOG_FLAG_MESSAGE : anchor_flag;

        // Everything you can change sits above the separator, everything that is
        // only there to read sits below it. Mixing the two makes the grey rows
        // look like they should do something when you press on them.
        const rg_gui_option_t options[] = {
            {0x00, _("Wi-Fi mode"),         "-",  RG_DIALOG_FLAG_NORMAL,  &wifi_mode_cb            },
            {0x00, _("Scan networks"),      NULL, RG_DIALOG_FLAG_NORMAL,  &wifi_scan_cb            },
            {0x00, _("Manage networks"),    NULL, RG_DIALOG_FLAG_NORMAL,  &wifi_manage_networks_cb },
            {0x00, _("Info"),               NULL, RG_DIALOG_FLAG_NORMAL,  &wifi_info_cb            },
            {0x14, _("File server"),        "-",  edit_flag,              &wifi_server_toggle_cb   },
            {0x13, _("Server pass"),        "-",  edit_flag,              &wifi_server_pass_cb     },
            RG_DIALOG_SEPARATOR,
            {0x12, _("Server user"),        "-",  info_flag,              &wifi_status_cb          },
            {0x10, _("Network"),            "-",  RG_DIALOG_FLAG_MESSAGE, &wifi_status_cb          },
            {0x11, _("IP address"),         "-",  ip_flag,                &wifi_status_cb          },
            {0x15, _("Web name"),           "-",  webname_flag,           &wifi_status_cb          },
            RG_DIALOG_END,
        };
        rg_gui_dialog(option->label, options, 0);
    }
    return RG_DIALOG_VOID;
}
#endif

static rg_gui_event_t bt_scan_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        rg_gui_draw_message(_("Scanning for controller..."));
        if (!rg_bluetooth_init())
            rg_gui_alert(_("Error"), _("Failed to start Bluetooth"));
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t bt_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    bool enabled = rg_settings_get_boolean(NS_GLOBAL, RG_SETTING_BT_CONTROLLER, false);

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
    {
        enabled = !enabled;
        rg_settings_set_boolean(NS_GLOBAL, RG_SETTING_BT_CONTROLLER, enabled);
        rg_settings_commit();
        if (enabled)
        {
            rg_gui_draw_message(_("Starting Bluetooth..."));
            if (!rg_bluetooth_init())
            {
                rg_gui_alert(_("Error"), _("Failed to start Bluetooth"));
                rg_settings_set_boolean(NS_GLOBAL, RG_SETTING_BT_CONTROLLER, false);
                rg_settings_commit();
                enabled = false;
            }
        }
        else
        {
            rg_gui_draw_message(_("Stopping Bluetooth..."));
            rg_bluetooth_deinit();
        }
        strcpy(option->value, enabled ? _("On") : _("Off"));
        return RG_DIALOG_REDRAW;
    }

    strcpy(option->value, enabled ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t bt_status_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    switch (rg_bluetooth_get_state())
    {
        case RG_BT_STATE_CONNECTED:
            snprintf(option->value, 32, "%.31s", rg_bluetooth_get_device_name());
            break;
        case RG_BT_STATE_CONNECTING:
            strcpy(option->value, _("Connecting..."));
            break;
        case RG_BT_STATE_SCANNING:
            strcpy(option->value, _("Searching..."));
            break;
        default:
            strcpy(option->value, _("Off"));
            break;
    }
    return RG_DIALOG_VOID;
}

// Shows which physical buttons (usages) are bound to a retro-go key, e.g.
// "B7+B9" for the default L1+L2 pair, or "-" when unassigned.
static void bt_binding_str(uint32_t key, char *buf, size_t size)
{
    const uint32_t *map = rg_bluetooth_get_button_map();
    buf[0] = 0;
    for (int i = 0; i < RG_BT_MAX_BUTTONS; ++i)
        if (map[i] == key)
            snprintf(buf + strlen(buf), size - strlen(buf), "%sB%d", buf[0] ? "+" : "", i + 1);
    if (!buf[0]) // Directions are parsed from the hat/stick automatically
        strcpy(buf, (key & (RG_KEY_UP | RG_KEY_DOWN | RG_KEY_LEFT | RG_KEY_RIGHT)) ? "auto" : "-");
}

// One row per function: press ENTER, then press the desired controller
// button to (re)bind just that function. Device B cancels.
static rg_gui_event_t bt_bind_key_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    uint32_t key = (uint32_t)option->arg;

    if (event == RG_DIALOG_ENTER)
    {
        if (rg_bluetooth_get_state() != RG_BT_STATE_CONNECTED)
        {
            rg_gui_alert(_("Bluetooth"), _("Connect a controller first."));
        }
        else
        {
            char msg[96];
            snprintf(msg, sizeof(msg), "%s:\n\n[ %s ]\n\n%s", _("Press the button for"),
                     option->label, _("(B on device = cancel)"));
            rg_gui_draw_message(msg);
            rg_bluetooth_set_capture(true);

            int64_t deadline = rg_system_timer() + 10 * 1000000;
            // Wait for everything to be released first
            while ((rg_bluetooth_read_raw_buttons() || (rg_input_read_gamepad() & RG_KEY_B)) &&
                   rg_system_timer() < deadline)
                rg_task_delay(20);
            // Then wait for a press
            uint32_t raw = 0;
            while (rg_system_timer() < deadline)
            {
                raw = rg_bluetooth_read_raw_buttons();
                if (raw || (rg_input_read_gamepad() & RG_KEY_B))
                    break;
                rg_task_delay(20);
            }
            if (raw)
            {
                // Move semantics: the chosen button gets this function, and
                // the function disappears from wherever it was bound before.
                uint32_t map[RG_BT_MAX_BUTTONS];
                memcpy(map, rg_bluetooth_get_button_map(), sizeof(map));
                for (int i = 0; i < RG_BT_MAX_BUTTONS; ++i)
                    if (map[i] == key)
                        map[i] = 0;
                map[__builtin_ctz(raw)] = key;
                rg_bluetooth_set_button_map(map);
            }
            // Wait for release so the press doesn't leak into the menu
            int64_t rel = rg_system_timer() + 3 * 1000000;
            while ((rg_bluetooth_read_raw_buttons() || rg_input_read_gamepad()) && rg_system_timer() < rel)
                rg_task_delay(20);
            rg_bluetooth_set_capture(false);
        }
    }

    bt_binding_str(key, option->value, 16);
    return (event == RG_DIALOG_ENTER) ? RG_DIALOG_REDRAW : RG_DIALOG_VOID;
}

static rg_gui_event_t bt_remap_reset_cb(rg_gui_option_t *option, rg_gui_event_t event);

// Press a controller button first, then pick what it should do from a list.
// This is per-button (multiple buttons may share a function), and never
// touches the device's own buttons.
static rg_gui_event_t bt_assign_button_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    if (rg_bluetooth_get_state() != RG_BT_STATE_CONNECTED)
    {
        rg_gui_alert(_("Bluetooth"), _("Connect a controller first."));
        return RG_DIALOG_REDRAW;
    }

    static const struct { const char *name; uint32_t key; } funcs[] = {
        {"(nothing)", 0},
        {"A", RG_KEY_A}, {"B", RG_KEY_B},
        {"X / Bright+", RG_KEY_X}, {"Y / Bright-", RG_KEY_Y},
        {"L / Vol+", RG_KEY_L}, {"R / Vol-", RG_KEY_R},
        {"SELECT", RG_KEY_SELECT}, {"START", RG_KEY_START}, {"MENU", RG_KEY_MENU},
        {"UP", RG_KEY_UP}, {"DOWN", RG_KEY_DOWN}, {"LEFT", RG_KEY_LEFT}, {"RIGHT", RG_KEY_RIGHT},
    };

    rg_gui_draw_message(_("Press a button on the controller..."));
    rg_bluetooth_set_capture(true);

    int64_t deadline = rg_system_timer() + 10 * 1000000;
    while ((rg_bluetooth_read_raw_buttons() || (rg_input_read_gamepad() & RG_KEY_B)) &&
           rg_system_timer() < deadline)
        rg_task_delay(20);
    uint32_t raw = 0;
    while (rg_system_timer() < deadline)
    {
        raw = rg_bluetooth_read_raw_buttons();
        if (raw || (rg_input_read_gamepad() & RG_KEY_B))
            break;
        rg_task_delay(20);
    }
    // Wait for release, then let go of the input again so the user can
    // navigate the function list with the controller.
    int64_t rel = rg_system_timer() + 3 * 1000000;
    while ((rg_bluetooth_read_raw_buttons() || rg_input_read_gamepad()) && rg_system_timer() < rel)
        rg_task_delay(20);
    rg_bluetooth_set_capture(false);

    if (!raw)
        return RG_DIALOG_REDRAW;

    int usage = __builtin_ctz(raw) + 1;
    uint32_t current = rg_bluetooth_get_button_map()[usage - 1];

    rg_gui_option_t options[RG_COUNT(funcs) + 1];
    int selected = 0;
    for (size_t i = 0; i < RG_COUNT(funcs); ++i)
    {
        options[i] = (rg_gui_option_t){i, funcs[i].name, NULL, RG_DIALOG_FLAG_NORMAL, NULL};
        if (funcs[i].key == current && current != 0)
            selected = i;
    }
    options[RG_COUNT(funcs)] = (rg_gui_option_t)RG_DIALOG_END;

    char title[32];
    snprintf(title, sizeof(title), "%s B%d", _("Button"), usage);
    intptr_t sel = rg_gui_dialog(title, options, selected);
    if (sel >= 0 && sel < (intptr_t)RG_COUNT(funcs))
    {
        uint32_t map[RG_BT_MAX_BUTTONS];
        memcpy(map, rg_bluetooth_get_button_map(), sizeof(map));
        map[usage - 1] = funcs[sel].key;
        rg_bluetooth_set_button_map(map);
    }
    return RG_DIALOG_REDRAW;
}

// Guided setup: walks through every function with a countdown bar; the
// controller button pressed within the window gets bound. No press = keep
// the old binding. Any device button aborts without saving.
static rg_gui_event_t bt_setup_wizard_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    if (rg_bluetooth_get_state() != RG_BT_STATE_CONNECTED)
    {
        rg_gui_alert(_("Bluetooth"), _("Connect a controller first."));
        return RG_DIALOG_REDRAW;
    }

    const int W = rg_display_get_width();
    const int H = rg_display_get_height();
    const int bar_x = W / 2 - 130, bar_w = 260, bar_y = 170;
    const int64_t start_hold_us = 2 * 1000000;
    const int64_t step_window_us = 6 * 1000000;
    bool saved = false;

    rg_bluetooth_set_capture(true);

    // ---- Intro screen: explain, then hold any controller button 2s ----
    rg_gui_draw_rect(0, 0, W, H, 0, C_BLACK, C_BLACK);
    rg_gui_draw_text(0, 6, W, _("Button setup"), C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER | RG_TEXT_BIGGER);
    rg_gui_draw_text(0, 50, W, _("Each button gets a countdown bar."), C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_text(0, 66, W, _("Press the controller button you want"), C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_text(0, 82, W, _("before the bar runs out."), C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_text(0, 110, W, _("Hold any button for 2s to start"), C_GREEN, C_BLACK, RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_text(0, H - 18, W, _("Device button = cancel"), C_SILVER, C_BLACK, RG_TEXT_ALIGN_CENTER);

    int64_t held_since = 0;
    int prev_bar = -1;
    bool started = false, aborted = false;
    while (!started && !aborted)
    {
        if (rg_input_read_gamepad())
            aborted = true;
        else if (rg_bluetooth_read_raw_buttons())
        {
            int64_t now = rg_system_timer();
            if (held_since == 0)
                held_since = now;
            if (now - held_since >= start_hold_us)
                started = true;
            int bar = (int)((now - held_since) * bar_w / start_hold_us);
            if (bar != prev_bar)
            {
                rg_gui_draw_rect(bar_x, bar_y, RG_MIN(bar, bar_w), 8, 0, C_GREEN, C_GREEN);
                prev_bar = bar;
            }
        }
        else if (held_since != 0)
        {
            held_since = 0;
            prev_bar = -1;
            rg_gui_draw_rect(bar_x, bar_y, bar_w, 8, 0, C_BLACK, C_BLACK);
        }
        rg_task_delay(20);
    }

    if (started)
    {
        // additive: bind without clearing the key's other bindings (so L1+L2
        // can share L). direction: the D-pad/hat is parsed automatically, so
        // a direction press just confirms; a *button* press binds that button.
        static const struct { const char *name; uint32_t key; bool additive; bool direction; } steps[] = {
            {"UP", RG_KEY_UP, false, true}, {"DOWN", RG_KEY_DOWN, false, true},
            {"LEFT", RG_KEY_LEFT, false, true}, {"RIGHT", RG_KEY_RIGHT, false, true},
            {"A", RG_KEY_A, false, false}, {"B", RG_KEY_B, false, false},
            {"X", RG_KEY_X, false, false}, {"Y", RG_KEY_Y, false, false},
            {"L1", RG_KEY_L, false, false}, {"L2", RG_KEY_L, true, false},
            {"R1", RG_KEY_R, false, false}, {"R2", RG_KEY_R, true, false},
            {"SELECT", RG_KEY_SELECT, false, false}, {"START", RG_KEY_START, false, false},
        };
        const uint32_t dir_keys = RG_KEY_UP | RG_KEY_DOWN | RG_KEY_LEFT | RG_KEY_RIGHT;
        uint32_t map[RG_BT_MAX_BUTTONS];
        memcpy(map, rg_bluetooth_get_button_map(), sizeof(map));
        char text[48];

        for (size_t i = 0; i < RG_COUNT(steps) && !aborted; ++i)
        {
            rg_gui_draw_rect(0, 40, W, H - 40, 0, C_BLACK, C_BLACK);
            snprintf(text, sizeof(text), "%d / %d", (int)(i + 1), (int)RG_COUNT(steps));
            rg_gui_draw_text(0, 44, W, text, C_GRAY, C_BLACK, RG_TEXT_ALIGN_CENTER);
            rg_gui_draw_text(0, 70, W, _("Press the button for"), C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
            rg_gui_draw_text(0, 94, W, steps[i].name, C_GREEN, C_BLACK, RG_TEXT_ALIGN_CENTER | RG_TEXT_BIGGER);
            rg_gui_draw_text(0, H - 18, W, _("Device button = cancel"), C_SILVER, C_BLACK, RG_TEXT_ALIGN_CENTER);

            // Wait until everything is released (buttons and directions)
            int64_t rel = rg_system_timer() + 3 * 1000000;
            while ((rg_bluetooth_read_raw_buttons() || (rg_bluetooth_peek_gamepad() & dir_keys)) &&
                   rg_system_timer() < rel)
                rg_task_delay(20);

            // Countdown window
            int64_t end = rg_system_timer() + step_window_us;
            uint32_t raw = 0;
            bool auto_ok = false;
            prev_bar = -1;
            while (rg_system_timer() < end)
            {
                if (rg_input_read_gamepad()) { aborted = true; break; }
                raw = rg_bluetooth_read_raw_buttons();
                if (raw)
                    break;
                if (steps[i].direction && (rg_bluetooth_peek_gamepad() & steps[i].key))
                {
                    auto_ok = true; // D-pad/hat/stick: parsed automatically
                    break;
                }
                int bar = (int)((end - rg_system_timer()) * bar_w / step_window_us);
                if (bar != prev_bar)
                {
                    rg_gui_draw_rect(bar_x, bar_y, RG_MAX(bar, 0), 8, 0, C_GREEN, C_GREEN);
                    rg_gui_draw_rect(bar_x + RG_MAX(bar, 0), bar_y, bar_w - RG_MAX(bar, 0), 8, 0, C_BLACK, C_BLACK);
                    prev_bar = bar;
                }
                rg_task_delay(20);
            }
            if (aborted)
                break;

            rg_gui_draw_rect(bar_x, bar_y, bar_w, 8, 0, C_BLACK, C_BLACK);
            if (raw)
            {
                int usage = __builtin_ctz(raw) + 1;
                if (!steps[i].additive)
                    for (int j = 0; j < RG_BT_MAX_BUTTONS; ++j)
                        if (map[j] == steps[i].key)
                            map[j] = 0;
                map[usage - 1] = steps[i].key;
                snprintf(text, sizeof(text), "%s  >  B%d", steps[i].name, usage);
                rg_gui_draw_text(0, 130, W, text, C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
            }
            else if (auto_ok)
            {
                snprintf(text, sizeof(text), "%s  OK", steps[i].name);
                rg_gui_draw_text(0, 130, W, text, C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER);
            }
            else
            {
                rg_gui_draw_text(0, 130, W, _("Skipped (unchanged)"), C_GRAY, C_BLACK, RG_TEXT_ALIGN_CENTER);
            }
            rg_task_delay(600);
        }

        if (!aborted)
        {
            rg_bluetooth_set_button_map(map);
            saved = true;
            rg_gui_draw_rect(0, 40, W, H - 40, 0, C_BLACK, C_BLACK);
            rg_gui_draw_text(0, 100, W, _("Mapping saved!"), C_GREEN, C_BLACK, RG_TEXT_ALIGN_CENTER | RG_TEXT_BIGGER);
            rg_task_delay(1000);
        }
    }

    // Wait for release so nothing leaks into the menu
    int64_t rel = rg_system_timer() + 3 * 1000000;
    while ((rg_bluetooth_read_raw_buttons() || rg_input_read_gamepad()) && rg_system_timer() < rel)
        rg_task_delay(20);
    rg_bluetooth_set_capture(false);
    rg_display_force_redraw();
    if (!saved && aborted)
        rg_gui_alert(_("Button setup"), _("Cancelled, nothing changed."));
    return RG_DIALOG_REDRAW;
}

// Remap submenu: every function on its own row with its current binding
static rg_gui_event_t bt_remap_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_gui_option_t options[] = {
            {0, _("Set up all buttons"), NULL, RG_DIALOG_FLAG_NORMAL, &bt_setup_wizard_cb},
            {0, _("Assign a button"),    NULL, RG_DIALOG_FLAG_NORMAL, &bt_assign_button_cb},
            RG_DIALOG_SEPARATOR,
            {RG_KEY_UP,     "UP",     "-",  RG_DIALOG_FLAG_NORMAL, &bt_bind_key_cb},
            {RG_KEY_DOWN,   "DOWN",   "-",  RG_DIALOG_FLAG_NORMAL, &bt_bind_key_cb},
            {RG_KEY_LEFT,   "LEFT",   "-",  RG_DIALOG_FLAG_NORMAL, &bt_bind_key_cb},
            {RG_KEY_RIGHT,  "RIGHT",  "-",  RG_DIALOG_FLAG_NORMAL, &bt_bind_key_cb},
            {RG_KEY_A,      "A",      "-",  RG_DIALOG_FLAG_NORMAL, &bt_bind_key_cb},
            {RG_KEY_B,      "B",      "-",  RG_DIALOG_FLAG_NORMAL, &bt_bind_key_cb},
            {RG_KEY_X,      "X / Bright+", "-", RG_DIALOG_FLAG_NORMAL, &bt_bind_key_cb},
            {RG_KEY_Y,      "Y / Bright-", "-", RG_DIALOG_FLAG_NORMAL, &bt_bind_key_cb},
            {RG_KEY_L,      "L / Vol+",    "-", RG_DIALOG_FLAG_NORMAL, &bt_bind_key_cb},
            {RG_KEY_R,      "R / Vol-",    "-", RG_DIALOG_FLAG_NORMAL, &bt_bind_key_cb},
            {RG_KEY_SELECT, "SELECT", "-",  RG_DIALOG_FLAG_NORMAL, &bt_bind_key_cb},
            {RG_KEY_START,  "START",  "-",  RG_DIALOG_FLAG_NORMAL, &bt_bind_key_cb},
            RG_DIALOG_SEPARATOR,
            {0, _("Reset mapping"), NULL, RG_DIALOG_FLAG_NORMAL, &bt_remap_reset_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(option->label, options, 0);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

// One button on the controller-test screen: lights up green while pressed
static void bt_test_draw_button(int x, int y, int w, int h, const char *label, bool on)
{
    rg_color_t fill = on ? C_GREEN : 0x2124; // Very dark gray when idle
    rg_gui_draw_rect(x, y, w, h, 1, on ? C_WHITE : C_GRAY, fill);
    if (label && label[0])
    {
        int ty = y + (h - 12) / 2;
        rg_gui_draw_text(x + 2, RG_MAX(ty, y + 1), w - 4, label,
                         on ? C_BLACK : C_SILVER, fill, RG_TEXT_ALIGN_CENTER);
    }
}

// Visual controller test: draws a stylized gamepad, pressed buttons light up.
// The shoulder buttons show the physical L1/L2/R1/R2 (raw usages 7-10), the
// rest shows the mapped functions. Exit by holding A on the controller for
// 5 seconds, or with any button on the device itself.
static rg_gui_event_t bt_test_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event != RG_DIALOG_ENTER)
        return RG_DIALOG_VOID;

    const int W = rg_display_get_width();  // Visible area (283x234 on this panel)
    const int H = rg_display_get_height();
    const int cx = W / 2;
    const int64_t hold_us = 2 * 1000000;
    const rg_color_t C_BODY = 0x18C3; // Near-black panel color for the gamepad body

    // Layout anchors, all relative to the visible area
    const int body_x = cx - 130, body_w = 260;
    const int body_y = 46, body_h = 132;
    const int dpad_cx = body_x + 52;   // D-pad cluster center (left)
    const int face_cx = body_x + 208;  // Face cluster center (right)
    const int clus_cy = body_y + 84;   // Vertical center of both clusters
    const int bar_y = body_y + body_h + 8;

    rg_bluetooth_set_capture(true);

    // Static background
    rg_gui_draw_rect(0, 0, W, H, 0, C_BLACK, C_BLACK);
    rg_gui_draw_text(0, 4, W, _("Controller test"), C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER | RG_TEXT_BIGGER);
    const char *name = rg_bluetooth_get_device_name();
    rg_gui_draw_text(0, 32, W, name[0] ? name : _("(no controller)"), C_GRAY, C_BLACK, RG_TEXT_ALIGN_CENTER);
    // Gamepad body with a subtle double outline
    rg_gui_draw_rect(body_x, body_y, body_w, body_h, 1, C_DIM_GRAY, C_BODY);
    rg_gui_draw_rect(body_x + 2, body_y + 2, body_w - 4, body_h - 4, 1, 0x39E7, C_NONE);
    // D-pad center block (static): connects the four arrows into a cross
    rg_gui_draw_rect(dpad_cx - 12, clus_cy - 12, 24, 24, 0, C_BODY, 0x2124);
    // Exit hint
    rg_gui_draw_text(0, bar_y + 14, W, _("Hold A for 2 seconds to exit"), C_SILVER, C_BLACK, RG_TEXT_ALIGN_CENTER);

    uint32_t prev_keys = 0xFFFFFFFF, prev_raw = 0xFFFFFFFF;
    int64_t a_held_since = 0;
    int prev_bar = -1;

    while (true)
    {
        if (rg_input_read_gamepad()) // BLE reads 0 while capturing: device buttons only
            break;

        uint32_t keys = rg_bluetooth_peek_gamepad();
        uint32_t raw = rg_bluetooth_read_raw_buttons();

        if (keys != prev_keys || raw != prev_raw)
        {
            prev_keys = keys;
            prev_raw = raw;
            // Shoulders: physical L1/L2/R1/R2 (usages 7,9 / 8,10), L2/R2 on top
            bt_test_draw_button(body_x + 8, body_y + 6, 52, 13, "L2", raw & (1 << 8));
            bt_test_draw_button(body_x + 8, body_y + 23, 52, 13, "L1", raw & (1 << 6));
            bt_test_draw_button(body_x + body_w - 60, body_y + 6, 52, 13, "R2", raw & (1 << 9));
            bt_test_draw_button(body_x + body_w - 60, body_y + 23, 52, 13, "R1", raw & (1 << 7));
            // D-pad cross (left cluster)
            bt_test_draw_button(dpad_cx - 12, clus_cy - 38, 24, 24, "", keys & RG_KEY_UP);
            bt_test_draw_button(dpad_cx - 12, clus_cy + 14, 24, 24, "", keys & RG_KEY_DOWN);
            bt_test_draw_button(dpad_cx - 38, clus_cy - 12, 24, 24, "", keys & RG_KEY_LEFT);
            bt_test_draw_button(dpad_cx + 14, clus_cy - 12, 24, 24, "", keys & RG_KEY_RIGHT);
            // Face buttons (right cluster, Nintendo positions)
            bt_test_draw_button(face_cx - 12, clus_cy - 38, 24, 24, "X", keys & RG_KEY_X);
            bt_test_draw_button(face_cx - 12, clus_cy + 14, 24, 24, "B", keys & RG_KEY_B);
            bt_test_draw_button(face_cx - 38, clus_cy - 12, 24, 24, "Y", keys & RG_KEY_Y);
            bt_test_draw_button(face_cx + 14, clus_cy - 12, 24, 24, "A", keys & RG_KEY_A);
            // Center cluster
            bt_test_draw_button(cx - 38, clus_cy - 8, 34, 15, "SEL", keys & RG_KEY_SELECT);
            bt_test_draw_button(cx + 4, clus_cy - 8, 34, 15, "STA", keys & RG_KEY_START);
        }

        // Hold-A-to-exit with a progress bar under the controller body
        if (keys & RG_KEY_A)
        {
            int64_t now = rg_system_timer();
            if (a_held_since == 0)
                a_held_since = now;
            if (now - a_held_since >= hold_us)
                break;
            int bar = (int)((now - a_held_since) * body_w / hold_us);
            if (bar != prev_bar)
            {
                rg_gui_draw_rect(body_x, bar_y, bar, 6, 0, C_GREEN, C_GREEN);
                prev_bar = bar;
            }
        }
        else if (a_held_since != 0)
        {
            a_held_since = 0;
            prev_bar = -1;
            rg_gui_draw_rect(body_x, bar_y, body_w, 6, 0, C_BLACK, C_BLACK);
        }

        rg_task_delay(30);
    }

    // Wait for release so the exit press doesn't trigger the menu
    rg_input_wait_for_key(RG_KEY_ANY, false, 1000);
    while (rg_bluetooth_peek_gamepad() & RG_KEY_A)
        rg_task_delay(20);
    rg_bluetooth_set_capture(false);
    rg_display_force_redraw();
    return RG_DIALOG_REDRAW;
}

static rg_gui_event_t bt_remap_reset_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        if (rg_gui_confirm(_("Bluetooth"), _("Reset button mapping to default?"), false))
        {
            rg_bluetooth_reset_button_map();
            rg_gui_alert(_("Bluetooth"), _("Mapping reset."));
        }
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t bt_forget_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        if (rg_gui_confirm(_("Forget pairings"), _("Remove all paired controllers?"), false))
        {
            rg_bluetooth_forget_bonds();
            rg_gui_alert(_("Bluetooth"), _("Pairings removed."));
        }
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t bluetooth_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_gui_option_t options[] = {
            {0, _("Scan for controller"), NULL, RG_DIALOG_FLAG_NORMAL, &bt_scan_cb},
            {0, _("Controller"),      "-",  RG_DIALOG_FLAG_NORMAL,  &bt_toggle_cb},
            {0, _("Test controller"), NULL, RG_DIALOG_FLAG_NORMAL,  &bt_test_cb},
            {0, _("Remap buttons"),   NULL, RG_DIALOG_FLAG_NORMAL,  &bt_remap_cb},
            {0, _("Forget pairings"), NULL, RG_DIALOG_FLAG_NORMAL,  &bt_forget_cb},
            RG_DIALOG_SEPARATOR,
            {0, _("Status"),          "-",  RG_DIALOG_FLAG_MESSAGE, &bt_status_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(option->label, options, 0);
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t app_options_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_app_t *app = rg_system_get_app();
        rg_gui_option_t options[16] = {0};
        if (app->handlers.options)
            app->handlers.options(options);
        rg_display_force_redraw();
        rg_gui_dialog(option->label, options, 0);
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t service_cb(rg_gui_option_t *option, rg_gui_event_t event);
static rg_gui_event_t debug_menu_cb(rg_gui_option_t *option, rg_gui_event_t event);
static rg_gui_event_t reset_settings_cb(rg_gui_option_t *option, rg_gui_event_t event);
static rg_gui_event_t about_popup_cb(rg_gui_option_t *option, rg_gui_event_t event);

static rg_gui_event_t battery_warn_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    const int thresholds[] = {0, 3100, 3200, 3300, 3400, 3500, 3600, 3700};
    const int count = 8;
    int current = rg_settings_get_number(NS_GLOBAL, "BattWarnV", 3300);
    int index = 0;

    for (int i = 0; i < count; i++)
        if (thresholds[i] == current)
            index = i;

    if (event == RG_DIALOG_PREV && --index < 0) index = count - 1;
    if (event == RG_DIALOG_NEXT && ++index >= count) index = 0;

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
        rg_settings_set_number(NS_GLOBAL, "BattWarnV", thresholds[index]);

    int val = rg_settings_get_number(NS_GLOBAL, "BattWarnV", 3300);
    if (val == 0)
        strcpy(option->value, _("Off"));
    else
        sprintf(option->value, "%.1fV", val / 1000.0f);

    return RG_DIALOG_VOID;
}

// Trims the ADC/divider reading against a multimeter measurement, for boards
// where the resistor divider isn't exactly the theoretical RG_BATTERY_VDIV_RATIO.
static rg_gui_event_t battery_cal_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int pct = rg_input_get_battery_cal();
    if (event == RG_DIALOG_PREV) rg_input_set_battery_cal(pct - 1);
    if (event == RG_DIALOG_NEXT) rg_input_set_battery_cal(pct + 1);

    rg_battery_t battery;
    if (rg_input_read_battery_raw(&battery))
        sprintf(option->value, "%d%% (%.2fV)", rg_input_get_battery_cal(), battery.volts);
    else
        sprintf(option->value, "%d%%", rg_input_get_battery_cal());

    return RG_DIALOG_VOID;
}

void rg_gui_options_menu(void)
{
    const rg_app_t *app_check = rg_system_get_app();

    // ===== BlockBoy: tabbed menu for launcher =====
    if (app_check->isLauncher)
    {
        static const char *const tab_names[] = { "General", "System" };

        rg_gui_menu_entry_t entries[48] = {0};
        int n = 0;

        // -------- GENERAL (tab 0) -- everything the user configures --------
        #if RG_SCREEN_BACKLIGHT
        entries[n++] = (rg_gui_menu_entry_t){ 0, "Brightness", true,  {0, _("Brightness"),     "-", RG_DIALOG_FLAG_NORMAL, &brightness_update_cb} };
        #endif
        entries[n++] = (rg_gui_menu_entry_t){ 0, "Volume",     true,  {0, _("Volume"),         "-", RG_DIALOG_FLAG_NORMAL, &volume_update_cb} };
        entries[n++] = (rg_gui_menu_entry_t){ 0, "AudioOut",   true,  {0, _("Audio out"),      "-", RG_DIALOG_FLAG_NORMAL, &audio_update_cb} };
        entries[n++] = (rg_gui_menu_entry_t){ 1, "BattWarn",   true,  {0, _("Batt. warning"),  "-", RG_DIALOG_FLAG_NORMAL, &battery_warn_cb} };
        entries[n++] = (rg_gui_menu_entry_t){ 1, "BattCal",    false, {0, _("Batt. calibration"), "-", RG_DIALOG_FLAG_NORMAL, &battery_cal_cb} };
        entries[n++] = (rg_gui_menu_entry_t){ 0, "FontType",   false, {0, _("Font type"),      "-",  RG_DIALOG_FLAG_NORMAL, &font_type_cb} };
        entries[n++] = (rg_gui_menu_entry_t){ 0, "Theme",      true,  {0, _("Menu theme"),     "-",  RG_DIALOG_FLAG_NORMAL, &theme_cb} };
        entries[n++] = (rg_gui_menu_entry_t){ 0, "ColorTune",  false, {0, _("Color settings"),   NULL, RG_DIALOG_FLAG_NORMAL, &color_menu_cb} };
        entries[n++] = (rg_gui_menu_entry_t){ 0, "ScreenPos",  false, {0, _("Screen position"),NULL, RG_DIALOG_FLAG_NORMAL, &screen_position_menu_cb} };

        // -------- SYSTEM (tab 1) -- language/time, then connectivity --------
#if RG_LANG_MAX > 1
        // Only worth showing when there is something to choose between.
        entries[n++] = (rg_gui_menu_entry_t){ 1, "Language",   true,  {0, _("Language"),       "-", RG_DIALOG_FLAG_NORMAL, &language_cb} };
#endif
        entries[n++] = (rg_gui_menu_entry_t){ 1, "Clock",      true,  {0, _("Clock"),          NULL, RG_DIALOG_FLAG_NORMAL, &clock_menu_cb} };
        #ifdef RG_ENABLE_NETWORKING
        entries[n++] = (rg_gui_menu_entry_t){ 1, "WifiOptions", true, {0, _("Wi-Fi"),          NULL, RG_DIALOG_FLAG_NORMAL, &wifi_cb} };
        // Hidden by default; the Shop menu can only toggle entries that live in
        // THIS list, so it has to be a tabbed entry rather than a row inside the
        // Wi-Fi submenu.
        entries[n++] = (rg_gui_menu_entry_t){ 1, "WifiAP",     false, {0, _("Wi-Fi AP"),       NULL, RG_DIALOG_FLAG_NORMAL, &wifi_access_point_cb} };
        #endif
        if (rg_bluetooth_supported())
        entries[n++] = (rg_gui_menu_entry_t){ 1, "BtOptions",   true, {0, _("Bluetooth"),      NULL, RG_DIALOG_FLAG_NORMAL, &bluetooth_cb} };

        // -------- Launcher handler vult tab 0 en tab 1 --------
        if (app_check->handlers.tabbed_options)
        {
            app_check->handlers.tabbed_options(&entries[n]);
            while (n < (int)(RG_COUNT(entries) - 12) && entries[n].tab_id >= 0)
                n++;
        }

        // -------- SYSTEM (tab 1) -- beheer (onderaan) --------
        entries[n++] = (rg_gui_menu_entry_t){ 1, "About",         true,  {0, _("About"),          NULL, RG_DIALOG_FLAG_NORMAL, &about_popup_cb} };
        entries[n++] = (rg_gui_menu_entry_t){ 1, "DebugMenu",     false, {0, _("Debug menu"),     NULL, RG_DIALOG_FLAG_NORMAL, &debug_menu_cb} };
        entries[n++] = (rg_gui_menu_entry_t){ 1, "ResetSettings", false, {0, _("Reset settings"), NULL, RG_DIALOG_FLAG_NORMAL, &reset_settings_cb} };
        entries[n++] = (rg_gui_menu_entry_t){ 1, NULL,            true,  {0, _("Service..."),     NULL, RG_DIALOG_FLAG_NORMAL, &service_cb} };
        entries[n++] = (rg_gui_menu_entry_t)RG_MENU_ENTRY_END;

        rg_audio_set_mute(true);
        rg_gui_tabbed_dialog(_("Settings"), tab_names, 2, entries, 0);
        rg_settings_commit();
        rg_audio_set_mute(false);
        return;
    }

    // Service-only entries (Color settings, Screen position): hidden by default,
    // toggle via Shop menu. Runtime flag evaluation requires C99 local array init.
    int color_flag  = rg_gui_get_option_visible("ColorTune", false) ? RG_DIALOG_FLAG_NORMAL : RG_DIALOG_FLAG_HIDDEN;
    int screen_flag = rg_gui_get_option_visible("ScreenPos", false) ? RG_DIALOG_FLAG_NORMAL : RG_DIALOG_FLAG_HIDDEN;
    rg_gui_option_t options[16] = {
        #if RG_SCREEN_BACKLIGHT
        {0, _("Brightness"),      "-",  RG_DIALOG_FLAG_NORMAL, &brightness_update_cb},
        #endif
        {0, _("Color settings"),    NULL, color_flag,            &color_menu_cb},
        {0, _("Screen position"), NULL, screen_flag,           &screen_position_menu_cb},
        {0, _("Volume"),          "-",  RG_DIALOG_FLAG_NORMAL, &volume_update_cb},
        {0, _("Audio out"),       "-",  RG_DIALOG_FLAG_NORMAL, &audio_update_cb},
        {0, _("Batt. warning"),   "-",  RG_DIALOG_FLAG_NORMAL, &battery_warn_cb},
        RG_DIALOG_END,
    };
    const rg_gui_option_t misc_options[] = {
        {0, _("Launcher options"), NULL, RG_DIALOG_FLAG_NORMAL, &app_options_cb},
        {0, _("Font type"),     "-", RG_DIALOG_FLAG_NORMAL, &font_type_cb},
        {0, _("Theme"),         "-", RG_DIALOG_FLAG_NORMAL, &theme_cb},
        {0, _("Clock"),         NULL, RG_DIALOG_FLAG_NORMAL, &clock_menu_cb},
#if RG_LANG_MAX > 1
        {0, _("Language"),      "-", RG_DIALOG_FLAG_NORMAL, &language_cb},
#endif
        #ifdef RG_GPIO_LED // Only show disk LED option if disk LED GPIO pin is defined
        {0, _("LED options"),   NULL, RG_DIALOG_FLAG_NORMAL, &led_indicator_cb},
        #endif
        #ifdef RG_ENABLE_NETWORKING
        {0, _("Wi-Fi options"), NULL, RG_DIALOG_FLAG_NORMAL, &wifi_cb},
        #endif
        {0, _("Bluetooth"),     NULL, rg_bluetooth_supported() ? RG_DIALOG_FLAG_NORMAL : RG_DIALOG_FLAG_HIDDEN, &bluetooth_cb},
        RG_DIALOG_END,
    };
    int border_flag = rg_gui_get_option_visible("Border", false) ? RG_DIALOG_FLAG_NORMAL : RG_DIALOG_FLAG_HIDDEN;
    // Diagnostics, not something a customer needs while playing. Same
    // treatment as Border: set Show_OsdStats on the SD card to get it back.
    int stats_flag  = rg_gui_get_option_visible("OsdStats", false) ? RG_DIALOG_FLAG_NORMAL : RG_DIALOG_FLAG_HIDDEN;
    const rg_gui_option_t game_options[] = {
        {0, _("Scaling"),       "-", RG_DIALOG_FLAG_NORMAL, &scaling_update_cb},
        {0, _("Factor"),        "-", RG_DIALOG_FLAG_HIDDEN, &custom_zoom_cb},
        {0, _("FPS overlay"),   "-", stats_flag,            &osd_stats_cb},
        {0, _("Filter"),        "-", RG_DIALOG_FLAG_NORMAL, &filter_update_cb},
        {0, _("Border"),        "-", border_flag,           &border_update_cb},
        {0, _("Speed"),         "-", RG_DIALOG_FLAG_NORMAL, &speedup_update_cb},
        // {0, _("Misc options"),  NULL, RG_DIALOG_FLAG_NORMAL, &misc_options_cb},
        {0, _("Emulator options"), NULL, RG_DIALOG_FLAG_NORMAL, &app_options_cb},
        RG_DIALOG_END,
    };

    const rg_app_t *app = rg_system_get_app();
    if (app->isLauncher)
        memcpy(options + get_dialog_items_count(options), misc_options, sizeof(misc_options));
    else
        memcpy(options + get_dialog_items_count(options), game_options, sizeof(game_options));

    rg_audio_set_mute(true);

    rg_gui_dialog(_("Options"), options, 0);
    rg_settings_commit();

    rg_audio_set_mute(false);
}

void rg_gui_debug_menu(void)
{
    char screen_res[20], source_res[20], scaled_res[20];
    char stack_hwm[20], heap_free[20], block_free[20];
    char local_time[32], timezone[32], uptime[20];
    char battery_info[25], frame_time[32];
    char app_name[32], network_str[64];

    const rg_gui_option_t options[] = {
        {0, "Screen res", screen_res,   RG_DIALOG_FLAG_NORMAL, NULL},
        {0, "Source res", source_res,   RG_DIALOG_FLAG_NORMAL, NULL},
        {0, "Scaled res", scaled_res,   RG_DIALOG_FLAG_NORMAL, NULL},
        {0, "Stack HWM ", stack_hwm,    RG_DIALOG_FLAG_NORMAL, NULL},
        {0, "Heap free ", heap_free,    RG_DIALOG_FLAG_NORMAL, NULL},
        {0, "Block free", block_free,   RG_DIALOG_FLAG_NORMAL, NULL},
        {0, "App name  ", app_name,     RG_DIALOG_FLAG_NORMAL, NULL},
        {0, "Network   ", network_str,  RG_DIALOG_FLAG_NORMAL, NULL},
        {0, "Local time", local_time,   RG_DIALOG_FLAG_NORMAL, NULL},
        {0, "Timezone  ", timezone,     RG_DIALOG_FLAG_NORMAL, NULL},
        {0, "Uptime    ", uptime,       RG_DIALOG_FLAG_NORMAL, NULL},
        {0, "Battery   ", battery_info, RG_DIALOG_FLAG_NORMAL, NULL},
        {0, "Blit time ", frame_time,   RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_SEPARATOR,
        {0, "Overclock", "-", RG_DIALOG_FLAG_NORMAL, &overclock_update_cb},
        {1, "Reboot to firmware", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {2, "Clear cache    ", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {3, "Save screenshot", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {4, "Save trace", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {6, "Crash     ", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {7, "Log=debug ", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END
    };

    const rg_display_t *display = rg_display_get_info();
    rg_display_counters_t display_stats = rg_display_get_counters();
    rg_stats_t stats = rg_system_get_counters();
    time_t now = time(NULL);

    strftime(local_time, 32, "%F %T", localtime(&now));
    snprintf(timezone, 32, "%s", getenv("TZ") ?: "N/A");
    snprintf(screen_res, 20, "%dx%d", display->screen.width, display->screen.height);
    snprintf(source_res, 20, "%dx%d", display->source.width, display->source.height);
    snprintf(scaled_res, 20, "%dx%d", display->viewport.width, display->viewport.height);
    if (display_stats.totalFrames > 0)
    {
        int total = (float)display_stats.busyTime / display_stats.totalFrames / 1000.f;
        int block = (float)display_stats.blockTime / display_stats.totalFrames / 1000.f;
        snprintf(frame_time, 20, "%dms (block: %dms)", total, block);
    }
    else
        snprintf(frame_time, 20, "N/A");
    snprintf(stack_hwm, 20, "%d", stats.freeStackMain);
    snprintf(heap_free, 20, "%d+%d", stats.freeMemoryInt, stats.freeMemoryExt);
    snprintf(block_free, 20, "%d+%d", stats.freeBlockInt, stats.freeBlockExt);
    snprintf(app_name, 32, "%s", rg_system_get_app()->name);
    snprintf(uptime, 20, "%ds", (int)(rg_system_timer() / 1000000));

    rg_battery_t battery;
    if (rg_input_read_battery_raw(&battery))
        snprintf(battery_info, sizeof(battery_info), "%.2f%% | %.2fV", battery.level, battery.volts);
    else
        snprintf(battery_info, sizeof(battery_info), "N/A");

    rg_network_t net = rg_network_get_info();
    if (net.state == RG_NETWORK_DISABLED)
        snprintf(network_str, 64, "%s", "not available");
    else if (net.state == RG_NETWORK_CONNECTED)
        snprintf(network_str, 64, "%s\n%s", net.name, net.ip_addr);
    else if (net.state == RG_NETWORK_CONNECTING)
        snprintf(network_str, 64, "%s\n%s", net.name, "connecting...");
    else if (net.name[0])
        snprintf(network_str, 64, "%s\n%s", net.name, "disconnected");
    else
        snprintf(network_str, 64, "%s", "disconnected");

    switch (rg_gui_dialog("Debugging", options, 0))
    {
    case 1:
        rg_system_switch_app(RG_APP_FACTORY, 0, 0, 0);
        break;
    case 2:
        rg_storage_delete(RG_BASE_PATH_CACHE);
        rg_system_restart();
        break;
    case 3:
        rg_emu_screenshot(RG_STORAGE_ROOT "/screenshot.png", 0, 0);
        break;
    case 4:
        rg_system_save_trace(RG_STORAGE_ROOT "/trace.txt", 0);
        break;
    case 6:
        RG_PANIC("Crash test!");
        break;
    case 7:
        rg_system_set_log_level(RG_LOG_DEBUG);
        break;
    }
}

static rg_gui_event_t slot_select_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    rg_emu_slot_t *slot = (rg_emu_slot_t *)option->arg;
    if (event == RG_DIALOG_FOCUS_GAINED)
    {
        rg_image_t *preview = NULL;
        rg_color_t color = C_BLUE;
        size_t margin = 0; // TEXT_RECT("ABC", 0).height;
        size_t border = 3;
        char buffer[100];
        if (slot->is_used)
        {
            preview = rg_surface_load_image_file(slot->preview, 0);
            if (slot->is_lastused)
                snprintf(buffer, sizeof(buffer), "Slot %d (last used)", slot->id);
            else
                snprintf(buffer, sizeof(buffer), "Slot %d", slot->id);
        }
        else
        {
            snprintf(buffer, sizeof(buffer), "Slot %d is empty", slot->id);
            color = C_RED;
        }
        rg_gui_draw_image(0, margin, gui.screen_width, gui.screen_height - margin * 2, true, preview);
        rg_gui_draw_rect(0, margin, gui.screen_width, gui.screen_height - margin * 2, border, color, C_NONE);
        rg_gui_draw_rect(border, margin + border, gui.screen_width - border * 2, gui.style.font_height * 2 + 6, 0, C_BLACK, C_BLACK);
        rg_gui_draw_text(border + 60, margin + border + 5, gui.screen_width - border * 2 - 120, buffer, C_WHITE, C_BLACK, RG_TEXT_ALIGN_CENTER|RG_TEXT_BIGGER|RG_TEXT_NO_PADDING);
        rg_surface_free(preview);
    }
    else if (event == RG_DIALOG_ENTER)
    {
        return RG_DIALOG_SELECT;
    }
    return RG_DIALOG_VOID;
    #undef draw_status
}

int rg_gui_savestate_menu(const char *title, const char *rom_path)
{
    rg_emu_states_t *savestates = rg_emu_get_states(rom_path, 4);
    const rg_gui_option_t choices[] = {
        {(intptr_t)&savestates->slots[0], _("Slot 0"), NULL, RG_DIALOG_FLAG_NORMAL, &slot_select_cb},
        {(intptr_t)&savestates->slots[1], _("Slot 1"), NULL, RG_DIALOG_FLAG_NORMAL, &slot_select_cb},
        {(intptr_t)&savestates->slots[2], _("Slot 2"), NULL, RG_DIALOG_FLAG_NORMAL, &slot_select_cb},
        {(intptr_t)&savestates->slots[3], _("Slot 3"), NULL, RG_DIALOG_FLAG_NORMAL, &slot_select_cb},
        RG_DIALOG_END
    };

    intptr_t ret = rg_gui_dialog(title, choices, savestates->lastused ? savestates->lastused->id : 0);
    int slot = (ret == RG_DIALOG_CANCELLED) ? -1 : ((rg_emu_slot_t *)ret)->id;
    free(savestates);
    return slot;
}

#define SHOWCASE_SEL_FILE RG_BASE_PATH_CONFIG "/favorite.txt"
#define SHOWCASE_ROT_FILE RG_BASE_PATH_CONFIG "/showcase_rotation.txt"

static char current_showcase_rom[64];
static char current_showcase_path[256];

void rg_gui_set_showcase_game_name(const char *path)
{
    strncpy(current_showcase_path, path ? path : "", sizeof(current_showcase_path) - 1);
    current_showcase_path[sizeof(current_showcase_path) - 1] = '\0';
    strncpy(current_showcase_rom, path ? rg_basename(path) : "", sizeof(current_showcase_rom) - 1);
    current_showcase_rom[sizeof(current_showcase_rom) - 1] = '\0';
    char *dot = strrchr(current_showcase_rom, '.');
    if (dot) *dot = '\0';
}

static bool showcase_in_favorites(const char *rom_path)
{
    if (!rom_path) return false;
    FILE *fp = fopen(SHOWCASE_SEL_FILE, "r");
    if (!fp) return false;
    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof(line), fp))
    {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        found = (strcmp(line, rom_path) == 0);
    }
    fclose(fp);
    return found;
}

static void showcase_toggle_favorite(const char *rom_path)
{
    if (!rom_path) return;
    FILE *fp = fopen(SHOWCASE_SEL_FILE, "r");
    char *buf = NULL;
    size_t buf_len = 0;
    if (fp)
    {
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        rewind(fp);
        buf = malloc(size + 2);
        if (buf) { buf_len = fread(buf, 1, size, fp); buf[buf_len] = '\0'; }
        fclose(fp);
    }
    size_t pathlen = strlen(rom_path);
    bool found = false;
    if (buf)
    {
        for (char *p = buf; *p; )
        {
            char *eol = strchr(p, '\n');
            size_t ll = eol ? (size_t)(eol - p) : strlen(p);
            while (ll && p[ll-1] == '\r') ll--;
            if (ll == pathlen && memcmp(p, rom_path, pathlen) == 0) { found = true; break; }
            p += eol ? (size_t)(eol - p + 1) : ll;
        }
    }
    fp = fopen(SHOWCASE_SEL_FILE, "w");
    if (!fp) { rg_storage_mkdir(RG_BASE_PATH_CONFIG); fp = fopen(SHOWCASE_SEL_FILE, "w"); }
    if (fp)
    {
        if (found && buf)
        {
            for (char *p = buf; *p; )
            {
                char *eol = strchr(p, '\n');
                size_t ll = eol ? (size_t)(eol - p) : strlen(p);
                while (ll && p[ll-1] == '\r') ll--;
                if (!(ll == pathlen && memcmp(p, rom_path, pathlen) == 0))
                    fprintf(fp, "%.*s\n", (int)ll, p);
                p += eol ? (size_t)(eol - p + 1) : ll;
            }
        }
        else
        {
            if (buf) fwrite(buf, 1, buf_len, fp);
            fprintf(fp, "%s\n", rom_path);
        }
        fclose(fp);
    }
    free(buf);
    remove(SHOWCASE_ROT_FILE);
}

// Stop showcase mode (used by the in-game menu): clears the rotation and disarms the
// daily on/off schedule so it can't sleep the device while the user is playing.
static void stop_showcase(void)
{
    rg_settings_set_number(NS_GLOBAL, "DisplayActive", 0);
    rg_settings_set_number(NS_GLOBAL, "DisplayElapsedSec", 0);
    rg_settings_set_number(NS_GLOBAL, "SchedEnable", 0);
    rg_settings_commit();
}

void rg_gui_game_menu(void)
{
    const char *rom_path = rg_system_get_app()->romPath;
    bool have_option_btn = rg_input_get_key_mapping(RG_KEY_OPTION);
    bool showcase = rg_settings_get_number(NS_GLOBAL, "DisplayActive", 0);
    char rom_title[64] = {0};
    if (showcase)
    {
        if (current_showcase_rom[0])
            strncpy(rom_title, current_showcase_rom, sizeof(rom_title) - 1);
        else if (rom_path)
        {
            strncpy(rom_title, rg_basename(rom_path), sizeof(rom_title) - 1);
            char *dot = strrchr(rom_title, '.');
            if (dot) *dot = '\0';
        }
    }

    if (showcase)
    {
        const char *fav_path = current_showcase_path[0] ? current_showcase_path : rom_path;
        bool in_fav = showcase_in_favorites(fav_path);
        const rg_gui_option_t choices[] = {
            {8000, _("Play this game"),         NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {9000, in_fav ? _("Remove from Favorites") : _("Add to Favorites"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {7000, _("Quit"),                   NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            RG_DIALOG_END
        };
        rg_audio_set_mute(true);
        suppress_status_bars = true;
        switch (rg_gui_dialog(rom_title[0] ? rom_title : NULL, choices, 0))
        {
            case 8000: stop_showcase(); break;
            case 9000: showcase_toggle_favorite(fav_path); break;
            case 7000: stop_showcase(); rg_system_exit(); break;
        }
        suppress_status_bars = false;
        rg_audio_set_mute(false);
        return;
    }

    rg_gui_option_t choices[12];
    int n = 0;
    choices[n++] = (rg_gui_option_t){1000, _("Save & Continue"), NULL, RG_DIALOG_FLAG_NORMAL, NULL};
    choices[n++] = (rg_gui_option_t){2000, _("Save & Quit"),     NULL, RG_DIALOG_FLAG_NORMAL, NULL};
    choices[n++] = (rg_gui_option_t){3001, _("Load game"),       NULL, RG_DIALOG_FLAG_NORMAL, NULL};
    choices[n++] = (rg_gui_option_t){3000, _("Reset"),           NULL, RG_DIALOG_FLAG_NORMAL, NULL};
    #ifdef RG_ENABLE_NETPLAY
    choices[n++] = (rg_gui_option_t){5000, _("Game Link"),       NULL, RG_DIALOG_FLAG_NORMAL, NULL};
    #endif
    choices[n++] = (rg_gui_option_t){5500, _("Options"),         NULL, RG_DIALOG_FLAG_NORMAL, NULL};
    choices[n++] = (rg_gui_option_t){7000, _("Quit"),            NULL, RG_DIALOG_FLAG_NORMAL, NULL};
    choices[n] = (rg_gui_option_t)RG_DIALOG_END;
    int slot, sel;

    rg_audio_set_mute(true);

    sel = rg_gui_dialog(NULL, choices, 0);

    if (sel == 3000)
    {
        const rg_gui_option_t choices[] = {
            {3002, _("Soft reset"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            {3003, _("Hard reset"), NULL, RG_DIALOG_FLAG_NORMAL, NULL},
            RG_DIALOG_END
        };
        sel = rg_gui_dialog(_("Reset Emulation?"), choices, 0);
    }

    switch (sel)
    {
        case 1000: if ((slot = rg_gui_savestate_menu(_("Save"), rom_path)) >= 0) rg_emu_save_state(slot); break;
        case 2000: if ((slot = rg_gui_savestate_menu(_("Save"), rom_path)) >= 0 && rg_emu_save_state(slot)) rg_system_exit(); break;
        case 3001: if ((slot = rg_gui_savestate_menu(_("Load"), rom_path)) >= 0) rg_emu_load_state(slot); break;
        case 3002: rg_emu_reset(false); break;
        case 3003: rg_emu_reset(true); break;
    #ifdef RG_ENABLE_NETPLAY
        case 5000: rg_netplay_quick_start(); break;
    #endif
        case 5500: rg_gui_options_menu(); break;
        case 7000: rg_system_exit(); break;
    }

    rg_audio_set_mute(false);
}

/* =================================================================================
 * Tabbed dialog (BlockBoy)
 *
 * Controls:
 *   UP/DOWN       -- move selection within current tab
 *   SELECT/START  -- switch to prev/next tab
 *   LEFT/RIGHT    -- send PREV/NEXT to the selected entry's callback
 *   A             -- send ENTER to the selected entry's callback
 *   B/OPTION/MENU -- cancel and close
 * ================================================================================= */

static rg_gui_menu_entry_t *current_tabbed_entries = NULL;
static bool tabbed_force_full_redraw = false;

bool rg_gui_get_option_visible(const char *vis_key, bool default_vis)
{
    if (!vis_key)
        return true;
    char setting_key[40];
    snprintf(setting_key, sizeof(setting_key), "Show_%s", vis_key);
    return rg_settings_get_boolean(NS_GLOBAL, setting_key, default_vis);
}

void rg_gui_set_option_visible(const char *vis_key, bool visible)
{
    if (!vis_key)
        return;
    char setting_key[40];
    snprintf(setting_key, sizeof(setting_key), "Show_%s", vis_key);
    rg_settings_set_boolean(NS_GLOBAL, setting_key, visible);
}

static int tabbed_count_entries(const rg_gui_menu_entry_t *entries)
{
    int n = 0;
    while (entries[n].tab_id >= 0)
        n++;
    return n;
}

static int tabbed_collect_visible(const rg_gui_menu_entry_t *entries, int total,
                                  int tab_id, int *visible)
{
    int count = 0;
    for (int i = 0; i < total; i++)
    {
        if (entries[i].tab_id != tab_id)
            continue;
        if ((entries[i].option.flags & RG_DIALOG_FLAG_MODE_MASK) == RG_DIALOG_FLAG_HIDDEN)
            continue;
        if (entries[i].vis_key
            && !rg_gui_get_option_visible(entries[i].vis_key, entries[i].default_vis))
            continue;
        visible[count++] = i;
    }
    return count;
}

static void tabbed_draw_row(int row_in_view, int visible_idx, int sel,
                            rg_gui_menu_entry_t *entries, const int *visible, int vcount,
                            int box_x, int box_w, int list_top, int row_height)
{
    if (visible_idx < 0 || visible_idx >= vcount) return;

    int idx = visible[visible_idx];
    rg_gui_option_t *opt = &entries[idx].option;
    int flag_mode = opt->flags & RG_DIALOG_FLAG_MODE_MASK;
    bool highlight = (flag_mode != RG_DIALOG_FLAG_SKIP) && (visible_idx == sel);

    rg_color_t fg, bg;
    if (highlight) {
        bg = gui.style.item_standard;
        fg = gui.style.box_background;
    } else {
        bg = gui.style.box_background;
        fg = (opt->flags == RG_DIALOG_FLAG_NORMAL) ? gui.style.item_standard : gui.style.item_disabled;
    }

    int ry = list_top + row_in_view * row_height;
    rg_gui_draw_rect(box_x + 4, ry, box_w - 8, row_height, 0, 0, bg);
    if (opt->label)
        rg_gui_draw_text(box_x + 10, ry + 3, 0, opt->label, fg, bg, RG_TEXT_ALIGN_LEFT);
    if (opt->value && *opt->value)
    {
        rg_rect_t vr = TEXT_RECT(opt->value, 0);
        rg_gui_draw_text(box_x + box_w - 10 - vr.width, ry + 3, 0,
                         opt->value, fg, bg, RG_TEXT_ALIGN_LEFT);
    }
}

static int tabbed_top_row(int sel, int vcount, int rows_per_screen)
{
    if (vcount > rows_per_screen && sel >= rows_per_screen)
        return sel - rows_per_screen + 1;
    return 0;
}

static int tabbed_box_y(const char *title)
{
    if (!title) return 12;
    const int title_h = gui.style.font_height + 6;
    return 4 + title_h + 2;
}

static void tabbed_draw_chrome(const char *title)
{
    const int margin = 12;
    const int box_y = tabbed_box_y(title);

    rg_gui_draw_rect(0, 0, gui.screen_width, gui.screen_height, 0, 0, gui.style.box_background);

    if (title)
    {
        rg_gui_draw_text(margin + 6, 4, gui.screen_width - margin * 2 - 12, title,
                         gui.style.box_border, gui.style.box_background, RG_TEXT_ALIGN_CENTER);
    }

    rg_gui_draw_rect(margin, box_y, gui.screen_width - margin * 2,
                     gui.screen_height - margin - box_y, 2, gui.style.box_border, gui.style.box_background);
}

static void tabbed_draw(const char *title, const char *const *tab_names, int num_tabs,
                        rg_gui_menu_entry_t *entries, int total,
                        int current_tab, int sel,
                        bool full_redraw, int prev_sel)
{
    const int font_height = gui.style.font_height;
    const int row_height = font_height + 6;
    const int tab_h = font_height + 10;
    const int margin = 12;

    const int box_w = gui.screen_width - margin * 2;
    const int box_x = margin;
    const int box_y = tabbed_box_y(title);
    const int box_h = gui.screen_height - margin - box_y;

    int visible[total ? total : 1];
    int vcount = tabbed_collect_visible(entries, total, current_tab, visible);

    const int tabs_y = box_y + 2;
    int y = tabs_y + tab_h + 3;

    const int list_top = y + 2;
    const int list_bottom = box_y + box_h - 12;
    const int rows_per_screen = (list_bottom - list_top) / row_height;

    int top_row = tabbed_top_row(sel, vcount, rows_per_screen);
    int prev_top_row = tabbed_top_row(prev_sel, vcount, rows_per_screen);

    bool partial_ok = !full_redraw && (top_row == prev_top_row);

    if (!partial_ok)
    {
        const int tab_area_x = box_x + 2;
        const int tab_area_w = box_w - 4;
        int tab_w_base = tab_area_w / num_tabs;
        for (int t = 0; t < num_tabs; t++)
        {
            int tx = tab_area_x + t * tab_w_base;
            int tw = (t == num_tabs - 1) ? (tab_area_w - tab_w_base * (num_tabs - 1)) : tab_w_base;
            bool active = (t == current_tab);
            rg_color_t bg = active ? gui.style.item_standard : gui.style.box_background;
            rg_color_t fg = active ? gui.style.box_background : gui.style.item_standard;
            rg_gui_draw_rect(tx, tabs_y, tw, tab_h, 0, 0, bg);
            rg_gui_draw_text(tx, tabs_y + 5, tw, tab_names[t], fg, bg, RG_TEXT_ALIGN_CENTER);
        }
        for (int t = 1; t < num_tabs; t++)
        {
            int sx = tab_area_x + t * tab_w_base;
            rg_gui_draw_rect(sx, tabs_y, 1, tab_h, 0, 0, gui.style.box_border);
        }

        rg_gui_draw_rect(box_x + 2, list_top, box_w - 4, list_bottom - list_top, 0, 0, gui.style.box_background);
        for (int row = 0; row < rows_per_screen && (top_row + row) < vcount; row++)
            tabbed_draw_row(row, top_row + row, sel, entries, visible, vcount,
                            box_x, box_w, list_top, row_height);

        if (vcount > rows_per_screen)
        {
            int sb_h = (rows_per_screen * (list_bottom - list_top)) / vcount;
            int sb_y = list_top + (top_row * (list_bottom - list_top)) / vcount;
            rg_gui_draw_rect(box_x + box_w - 6, sb_y, 3, sb_h, 0, 0, gui.style.item_disabled);
        }

        if (vcount == 0)
            rg_gui_draw_text(box_x, list_top + 20, box_w, "(no options)",
                             gui.style.item_disabled, gui.style.box_background, RG_TEXT_ALIGN_CENTER);
    }
    else
    {
        if (prev_sel != sel && prev_sel >= 0 && prev_sel >= top_row && prev_sel < top_row + rows_per_screen)
            tabbed_draw_row(prev_sel - top_row, prev_sel, sel, entries, visible, vcount,
                            box_x, box_w, list_top, row_height);
        if (sel >= top_row && sel < top_row + rows_per_screen)
            tabbed_draw_row(sel - top_row, sel, sel, entries, visible, vcount,
                            box_x, box_w, list_top, row_height);
    }
}

intptr_t rg_gui_tabbed_dialog(const char *title,
                              const char *const *tab_names,
                              int num_tabs,
                              rg_gui_menu_entry_t *entries,
                              int initial_tab)
{
    int total = tabbed_count_entries(entries);
    int current_tab = (initial_tab >= 0 && initial_tab < num_tabs) ? initial_tab : 0;
    int sel = 0;

    current_tabbed_entries = entries;

    char *text_buffer = calloc(total ? total : 1, 32);
    if (text_buffer)
    {
        char *p = text_buffer;
        for (int i = 0; i < total; i++)
        {
            rg_gui_option_t *opt = &entries[i].option;
            if (opt->value)
            {
                strncpy(p, opt->value, 31);
                p[31] = 0;
                opt->value = p;
                p += 32;
            }
        }
    }

    for (int i = 0; i < total; i++)
        if (entries[i].option.update_cb)
            entries[i].option.update_cb(&entries[i].option, RG_DIALOG_INIT);

    rg_audio_set_mute(true);
    rg_gui_draw_status_bars();

    tabbed_draw_chrome(title);

    rg_gui_event_t event = RG_DIALOG_VOID;
    uint32_t joystick = 0, joystick_old;
    uint64_t joystick_last = 0;
    bool redraw = true;
    intptr_t result_arg = 0;

    int prev_tab = -1;
    int prev_sel = -1;

    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
    rg_task_delay(80);

    while (event != RG_DIALOG_SELECT && event != RG_DIALOG_CANCEL)
    {
        if (redraw)
        {
            if (tabbed_force_full_redraw)
            {
                int v[total ? total : 1];
                int vc = tabbed_collect_visible(entries, total, current_tab, v);
                if (vc <= 0) sel = 0;
                else if (sel >= vc) sel = vc - 1;
                else if (sel < 0) sel = 0;

                tabbed_draw_chrome(title);
            }

            bool full = (prev_tab != current_tab) || tabbed_force_full_redraw;
            tabbed_force_full_redraw = false;
            tabbed_draw(title, tab_names, num_tabs, entries, total,
                        current_tab, sel, full, prev_sel);
            prev_tab = current_tab;
            prev_sel = sel;
            redraw = false;
        }

        joystick_old = ((rg_system_timer() - joystick_last) > 300000) ? 0 : joystick;
        joystick = rg_input_read_gamepad();
        event = RG_DIALOG_VOID;

        if (joystick ^ joystick_old)
        {
            int visible[total ? total : 1];
            int vcount = tabbed_collect_visible(entries, total, current_tab, visible);

            if (joystick & RG_KEY_SELECT) {
                current_tab = (current_tab - 1 + num_tabs) % num_tabs;
                sel = 0;
                redraw = true;
            }
            else if (joystick & RG_KEY_START) {
                current_tab = (current_tab + 1) % num_tabs;
                sel = 0;
                redraw = true;
            }
            else if (joystick & RG_KEY_UP) {
                if (vcount > 0) sel = (sel - 1 + vcount) % vcount;
                redraw = true;
            }
            else if (joystick & RG_KEY_DOWN) {
                if (vcount > 0) sel = (sel + 1) % vcount;
                redraw = true;
            }
            else if (joystick & (RG_KEY_B|RG_KEY_OPTION|RG_KEY_MENU)) {
                event = RG_DIALOG_CANCEL;
            }
            else if (vcount > 0 && sel < vcount)
            {
                rg_gui_option_t *opt = &entries[visible[sel]].option;
                bool active = (opt->flags & RG_DIALOG_FLAG_MODE_MASK) == RG_DIALOG_FLAG_NORMAL;
                rg_gui_callback_t cb = active ? opt->update_cb : NULL;

                if (joystick & RG_KEY_LEFT && cb) {
                    event = cb(opt, RG_DIALOG_PREV);
                    redraw = true;
                }
                else if (joystick & RG_KEY_RIGHT && cb) {
                    event = cb(opt, RG_DIALOG_NEXT);
                    redraw = true;
                }
                else if (joystick & RG_KEY_A && cb) {
                    event = cb(opt, RG_DIALOG_ENTER);
                    tabbed_force_full_redraw = true;
                    redraw = true;
                }
                else if (joystick & RG_KEY_A && active) {
                    result_arg = opt->arg;
                    event = RG_DIALOG_SELECT;
                }
            }

            joystick_last = rg_system_timer();
        }

        if (event == RG_DIALOG_REDRAW)
        {
            redraw = true;
            event = RG_DIALOG_VOID;
        }

        rg_task_delay(20);
    }

    if (text_buffer)
        free(text_buffer);

    // Wait for the closing key to be released before handing control back. Without
    // this the caller (the launcher loop) still sees MENU down and its key repeat --
    // whose deadline is long past by now -- fires immediately and reopens the menu.
    // rg_gui_dialog() does the same at its exit.
    rg_input_wait_for_key(joystick, false, 1000);

    rg_audio_set_mute(false);
    current_tabbed_entries = NULL;
    rg_display_force_redraw();

    return (event == RG_DIALOG_SELECT) ? result_arg : RG_DIALOG_CANCELLED;
}

/* =================================================================================
 * Shop Menu + PIN-protected Service entry
 * ================================================================================= */

#define SERVICE_PIN_LEN 4
static const uint32_t SERVICE_PIN[SERVICE_PIN_LEN] = {
    RG_KEY_UP, RG_KEY_UP, RG_KEY_DOWN, RG_KEY_A
};

static char pin_key_symbol(uint32_t k)
{
    switch (k) {
        case RG_KEY_UP:    return 'U';
        case RG_KEY_DOWN:  return 'D';
        case RG_KEY_LEFT:  return 'L';
        case RG_KEY_RIGHT: return 'R';
        case RG_KEY_A:     return 'A';
        case RG_KEY_B:     return 'B';
        default:           return '?';
    }
}

static void pin_dialog_draw(const uint32_t *entered, int pos)
{
    const int box_w = 240;
    const int box_h = 110;
    const int box_x = (gui.screen_width - box_w) / 2;
    const int box_y = (gui.screen_height - box_h) / 2;
    const int title_h = gui.style.font_height + 6;

    rg_gui_draw_rect(box_x, box_y, box_w, box_h, 2, gui.style.box_border, gui.style.box_background);
    rg_gui_draw_rect(box_x + 2, box_y + 2, box_w - 4, title_h, 0, 0, gui.style.item_standard);
    rg_gui_draw_text(box_x, box_y + 5, box_w, "Service", gui.style.box_background, gui.style.item_standard, RG_TEXT_ALIGN_CENTER);
    rg_gui_draw_text(box_x, box_y + title_h + 12, box_w, "Enter code",
                     gui.style.item_disabled, gui.style.box_background, RG_TEXT_ALIGN_CENTER);

    const int slot_w = 36, slot_h = 32, slot_gap = 10;
    const int total_w = slot_w * SERVICE_PIN_LEN + slot_gap * (SERVICE_PIN_LEN - 1);
    const int slot_x0 = box_x + (box_w - total_w) / 2;
    const int slot_y  = box_y + title_h + 36;

    for (int i = 0; i < SERVICE_PIN_LEN; i++)
    {
        int sx = slot_x0 + i * (slot_w + slot_gap);
        bool filled = (i < pos);
        bool cursor = (i == pos);
        rg_color_t bg = cursor ? gui.style.item_disabled : gui.style.box_background;
        rg_color_t fg = cursor ? gui.style.box_background : gui.style.item_standard;
        rg_gui_draw_rect(sx, slot_y, slot_w, slot_h, 1, gui.style.box_border, bg);
        char buf[2] = { filled ? pin_key_symbol(entered[i]) : '_', 0 };
        rg_gui_draw_text(sx, slot_y + 8, slot_w, buf, fg, bg, RG_TEXT_ALIGN_CENTER);
    }
}

bool rg_gui_verify_pin(void)
{
    uint32_t entered[SERVICE_PIN_LEN] = {0};
    int pos = 0;

    rg_audio_set_mute(true);
    pin_dialog_draw(entered, pos);
    rg_input_wait_for_key(RG_KEY_ALL, false, 1000);
    rg_task_delay(80);

    uint32_t joystick = 0, joystick_old = 0;
    bool cancelled = false;

    while (pos < SERVICE_PIN_LEN)
    {
        joystick_old = joystick;
        joystick = rg_input_read_gamepad();
        if (joystick && joystick != joystick_old)
        {
            uint32_t k = 0;
            if      (joystick & RG_KEY_UP)    k = RG_KEY_UP;
            else if (joystick & RG_KEY_DOWN)  k = RG_KEY_DOWN;
            else if (joystick & RG_KEY_LEFT)  k = RG_KEY_LEFT;
            else if (joystick & RG_KEY_RIGHT) k = RG_KEY_RIGHT;
            else if (joystick & RG_KEY_A)     k = RG_KEY_A;
            else if (joystick & RG_KEY_B)     { cancelled = true; break; }
            if (k) { entered[pos++] = k; pin_dialog_draw(entered, pos); }
        }
        rg_task_delay(20);
    }

    rg_audio_set_mute(false);
    if (cancelled) return false;

    bool match = true;
    for (int i = 0; i < SERVICE_PIN_LEN; i++)
        if (entered[i] != SERVICE_PIN[i]) { match = false; break; }
    if (!match) rg_gui_alert("Wrong code", "Try again.");
    return match;
}

static rg_gui_menu_entry_t *shop_target_entries = NULL;

// Options that live in the in-game menu instead of the settings tabs. The shop
// menu builds its list from the tabbed entries, so without this they could only
// be switched on by editing the settings file on the SD card -- no good for a
// customer who just wants to turn something on.
static const struct { const char *key; const char *label; bool default_vis; }
shop_ingame_options[] = {
    {"OsdStats", "FPS overlay", false},
    {"Border",   "Border",      false},
};

static bool shop_lookup_default(const char *vis_key)
{
    if (!vis_key) return true;
    for (size_t i = 0; i < RG_COUNT(shop_ingame_options); ++i)
        if (strcmp(shop_ingame_options[i].key, vis_key) == 0)
            return shop_ingame_options[i].default_vis;
    if (!shop_target_entries) return true;
    for (int i = 0; shop_target_entries[i].tab_id >= 0; i++)
        if (shop_target_entries[i].vis_key
            && strcmp(shop_target_entries[i].vis_key, vis_key) == 0)
            return shop_target_entries[i].default_vis;
    return true;
}

static rg_gui_event_t shop_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    const char *vis_key = (const char *)option->arg;
    bool current = rg_gui_get_option_visible(vis_key, shop_lookup_default(vis_key));
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT || event == RG_DIALOG_ENTER)
    {
        current = !current;
        rg_gui_set_option_visible(vis_key, current);
    }
    strcpy(option->value, current ? "[X]" : "[ ]");
    return RG_DIALOG_VOID;
}

static rg_gui_event_t shop_preset_reset_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER && shop_target_entries)
    {
        for (int i = 0; shop_target_entries[i].tab_id >= 0; i++)
            if (shop_target_entries[i].vis_key)
                rg_gui_set_option_visible(shop_target_entries[i].vis_key,
                                          shop_target_entries[i].default_vis);
        rg_gui_alert("Reset", "Defaults restored.");
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t shop_preset_all_on_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER && shop_target_entries)
    {
        for (int i = 0; shop_target_entries[i].tab_id >= 0; i++)
            if (shop_target_entries[i].vis_key)
                rg_gui_set_option_visible(shop_target_entries[i].vis_key, true);
        rg_gui_alert("All On", "All options enabled.");
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t shop_preset_all_off_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER && shop_target_entries)
    {
        for (int i = 0; shop_target_entries[i].tab_id >= 0; i++)
            if (shop_target_entries[i].vis_key)
                rg_gui_set_option_visible(shop_target_entries[i].vis_key, false);
        rg_gui_alert("All Off", "All options disabled.");
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

void rg_gui_shop_menu(rg_gui_menu_entry_t *entries)
{
    if (!entries) return;
    shop_target_entries = entries;

    int total = 0;
    while (entries[total].tab_id >= 0) total++;

    size_t cap = (size_t)(3 + 3 + total + 1 + RG_COUNT(shop_ingame_options) + 1);
    rg_gui_option_t *opts = calloc(cap, sizeof(rg_gui_option_t));
    if (!opts) { shop_target_entries = NULL; return; }
    int n = 0;

    opts[n++] = (rg_gui_option_t){ 0, "* All On",  NULL, RG_DIALOG_FLAG_NORMAL, &shop_preset_all_on_cb };
    opts[n++] = (rg_gui_option_t){ 0, "* All Off", NULL, RG_DIALOG_FLAG_NORMAL, &shop_preset_all_off_cb };
    opts[n++] = (rg_gui_option_t){ 0, "* Reset",   NULL, RG_DIALOG_FLAG_NORMAL, &shop_preset_reset_cb };

    static const char *const tab_headers[3] = { "-- Options --", "-- Launcher --", "-- About --" };

    for (int tab = 0; tab < 3; tab++)
    {
        int items_in_tab = 0;
        for (int i = 0; i < total; i++)
            if (entries[i].tab_id == tab && entries[i].vis_key) items_in_tab++;
        if (items_in_tab == 0) continue;

        opts[n++] = (rg_gui_option_t){ 0, tab_headers[tab], NULL, RG_DIALOG_FLAG_SKIP, NULL };
        for (int i = 0; i < total; i++)
        {
            if (entries[i].tab_id != tab || !entries[i].vis_key) continue;
            opts[n++] = (rg_gui_option_t){
                (intptr_t)entries[i].vis_key,
                entries[i].option.label,
                "[ ]",
                RG_DIALOG_FLAG_NORMAL,
                &shop_toggle_cb
            };
        }
    }

    opts[n++] = (rg_gui_option_t){ 0, "-- In game --", NULL, RG_DIALOG_FLAG_SKIP, NULL };
    for (size_t i = 0; i < RG_COUNT(shop_ingame_options); ++i)
        opts[n++] = (rg_gui_option_t){
            (intptr_t)shop_ingame_options[i].key,
            (char *)shop_ingame_options[i].label,
            "[ ]",
            RG_DIALOG_FLAG_NORMAL,
            &shop_toggle_cb
        };

    opts[n++] = (rg_gui_option_t)RG_DIALOG_END;

    rg_gui_dialog("Shop Menu", opts, 0);
    rg_settings_commit();
    free(opts);
    shop_target_entries = NULL;
}

static rg_gui_event_t service_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        if (rg_gui_verify_pin() && current_tabbed_entries)
            rg_gui_shop_menu(current_tabbed_entries);
        tabbed_force_full_redraw = true;
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t debug_menu_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        rg_gui_debug_menu();
        tabbed_force_full_redraw = true;
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t reset_settings_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        if (rg_gui_confirm(_("Reset all settings?"), NULL, false))
        {
            rg_storage_delete(RG_BASE_PATH_CACHE);
            rg_settings_reset();
            rg_system_restart();
        }
        tabbed_force_full_redraw = true;
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}

static rg_gui_event_t about_popup_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        // 7 lines of grey read-only text, one DOWN press from top to bottom.
        // Trick: the first and last line are FLAG_DISABLED (cursor anchors),
        // the middle 5 are FLAG_MESSAGE. The skip loop in rg_gui_dialog jumps
        // over MESSAGE entries, so DOWN on the first line lands straight on the
        // last one → the view shifts to the second "page" with the rest.
        // The DMG theme renders both colours identically (0x2A83), Classic a
        // little lighter for MESSAGE — both readable.
        const rg_gui_option_t options[] = {
            {0, "Version",   (char *)RG_PROJECT_VER,                       RG_DIALOG_FLAG_DISABLED, NULL},
            {0, "Target",    (char *)RG_TARGET_NAME,                       RG_DIALOG_FLAG_MESSAGE,  NULL},
            {0, "Website",   (char *)RG_PROJECT_WEBSITE,                   RG_DIALOG_FLAG_MESSAGE,  NULL},
            {0, "Project: BlockBoy by 0v1Tech",                     NULL, RG_DIALOG_FLAG_MESSAGE,  NULL},
            {0, "Based on Retro-Go by ducalex (GPL v3)",            NULL, RG_DIALOG_FLAG_MESSAGE,  NULL},
            {0, "Upstream: github.com/ducalex/retro-go",            NULL, RG_DIALOG_FLAG_MESSAGE,  NULL},
            {0, "Source: github.com/OviTech-BlockBoy/BlockBoy",     NULL, RG_DIALOG_FLAG_DISABLED, NULL},
            RG_DIALOG_END,
        };
        rg_gui_dialog(_("About"), options, 0);
        tabbed_force_full_redraw = true;
        return RG_DIALOG_REDRAW;
    }
    return RG_DIALOG_VOID;
}
