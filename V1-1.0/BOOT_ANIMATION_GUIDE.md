# Boot Animation Implementation Guide

This guide describes how the BlockBoy boot animation is implemented with menu options.

## Overview

- **3 animation options:** Blocks, Scroll, Off
- **Menu option:** In Launcher Options under "Boot animation"
- **Setting storage:** NS_GLOBAL so it can be read before launcher init

---

## Required Files

### 1. Logo Bitmap Header
**Location:** `components/retro-go/bitmaps/image_blockboy_logo.h`

This file contains the logo as an RGB565 pixel array. Format:
```c
static const struct {
    unsigned int width;
    unsigned int height;
    unsigned int bytes_per_pixel;
    uint16_t pixel_data[WIDTH * HEIGHT];
} image_blockboy_logo = {
    240, 58, 2,
    { 0x7C02, 0x7C02, ... }  // RGB565 pixel data
};
```

---

## Code Changes

### 2. launcher/main/gui.h

**Add enum after `preview_mode_t`:**
```c
typedef enum {
    BOOTANIM_BLOCKS = 0,
    BOOTANIM_SCROLL,
    BOOTANIM_OFF,
    BOOTANIM_COUNT
} bootanim_mode_t;
```

**Add variable to `retro_gui_t` struct:**
```c
typedef struct {
    // ... existing fields ...
    int scroll_mode;
    int bootanim;        // <-- NEW: add after scroll_mode
    int width;
    // ... rest of struct ...
} retro_gui_t;
```

---

### 3. launcher/main/gui.c

**Add setting constant (near other SETTING_ defines):**
```c
#define SETTING_BOOTANIM        "BootAnimation"
```

**Add loading in `gui_init()` (after scroll_mode):**
```c
void gui_init(bool cold_boot)
{
    gui = (retro_gui_t){
        // ... existing fields ...
        .scroll_mode  = rg_settings_get_number(NS_APP, SETTING_SCROLL_MODE, SCROLL_MODE_CENTER),
        .bootanim     = rg_settings_get_number(NS_GLOBAL, SETTING_BOOTANIM, BOOTANIM_BLOCKS),  // <-- NEW
        .width        = rg_display_get_width(),
        // ...
    };
}
```

**Add saving in `gui_save_config()` (before the for-loop):**
```c
void gui_save_config(void)
{
    // ... existing saves ...
    rg_settings_set_number(NS_APP, SETTING_STARTUP_MODE, gui.startup_mode);
    rg_settings_set_number(NS_GLOBAL, SETTING_BOOTANIM, gui.bootanim);  // <-- NEW
    for (int i = 0; i < gui.tabs_count; i++)
    // ...
}
```

---

### 4. launcher/main/main.c

**Add callback function (after `startup_app_cb`):**
```c
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
```

**Add menu option in `options_handler()`:**
```c
static void options_handler(rg_gui_option_t *dest)
{
    const rg_gui_option_t options[] = {
        {0, _("Color theme"),  "-", RG_DIALOG_FLAG_NORMAL, &color_theme_cb},
        {0, _("Preview"),      "-", RG_DIALOG_FLAG_NORMAL, &show_preview_cb},
        {0, _("Scroll mode"),  "-", RG_DIALOG_FLAG_NORMAL, &scroll_mode_cb},
        {0, _("Start screen"), "-", RG_DIALOG_FLAG_NORMAL, &start_screen_cb},
        {0, _("Boot animation"), "-", RG_DIALOG_FLAG_NORMAL, &bootanim_cb},  // <-- NEW
        {0, _("Hide tabs"),    "-", RG_DIALOG_FLAG_NORMAL, &toggle_tabs_cb},
        // ... rest ...
    };
    memcpy(dest, options, sizeof(options));
}
```

---

### 5. components/retro-go/rg_system.c

**Add include at the top (near other includes):**
```c
#include "bitmaps/image_blockboy_logo.h"
```

**Replace/add boot animation code in `rg_system_init()` (after `app.romPath = ...` line, before `rg_audio_init()`):**

```c
    // Boot animation setting: 0=Blocks, 1=Scroll, 2=Off
    int bootanim_mode = rg_settings_get_number(NS_GLOBAL, "BootAnimation", 0);

    if (app.isColdBoot && bootanim_mode != 2)  // Only on cold boot and if not disabled
    {
        // Original DMG "soup green" background color (RGB565 format)
        const rg_color_t DMG_BG = 0x7C02;  // #7B8210 - olive green background

        int screen_width = rg_display_get_width();
        int screen_height = rg_display_get_height();
        int logo_x = (screen_width - image_blockboy_logo.width) / 2;
        int logo_y = (screen_height - image_blockboy_logo.height) / 2;
        int total_pixels = image_blockboy_logo.width * image_blockboy_logo.height;

        // Clear screen
        rg_display_clear(DMG_BG);
        rg_task_delay(200);

        if (bootanim_mode == 0)  // Blocks animation
        {
            const int BLOCK_SIZE = 8;
            const int BLOCKS_PER_FRAME = 3;

            int blocks_x = (image_blockboy_logo.width + BLOCK_SIZE - 1) / BLOCK_SIZE;
            int blocks_y = (image_blockboy_logo.height + BLOCK_SIZE - 1) / BLOCK_SIZE;
            int total_blocks = blocks_x * blocks_y;

            uint16_t *display_buffer = rg_alloc(total_pixels * 2, MEM_FAST);
            uint8_t *block_revealed = rg_alloc(total_blocks, MEM_FAST);

            if (display_buffer && block_revealed)
            {
                for (int i = 0; i < total_pixels; i++)
                    display_buffer[i] = DMG_BG;
                for (int i = 0; i < total_blocks; i++)
                    block_revealed[i] = 0;

                unsigned int seed = esp_timer_get_time();
                int blocks_done = 0;

                while (blocks_done < total_blocks)
                {
                    for (int b = 0; b < BLOCKS_PER_FRAME && blocks_done < total_blocks; b++)
                    {
                        seed = seed * 1103515245 + 12345;
                        int block_idx = (seed >> 16) % total_blocks;

                        int attempts = 0;
                        while (block_revealed[block_idx] && attempts < total_blocks)
                        {
                            block_idx = (block_idx + 1) % total_blocks;
                            attempts++;
                        }

                        if (!block_revealed[block_idx])
                        {
                            int bx = (block_idx % blocks_x) * BLOCK_SIZE;
                            int by = (block_idx / blocks_x) * BLOCK_SIZE;

                            for (int py = 0; py < BLOCK_SIZE && (by + py) < (int)image_blockboy_logo.height; py++)
                            {
                                for (int px = 0; px < BLOCK_SIZE && (bx + px) < (int)image_blockboy_logo.width; px++)
                                {
                                    int idx = (by + py) * image_blockboy_logo.width + (bx + px);
                                    display_buffer[idx] = image_blockboy_logo.pixel_data[idx];
                                }
                            }

                            block_revealed[block_idx] = 1;
                            blocks_done++;
                        }
                    }

                    rg_display_write_rect(logo_x, logo_y,
                                          image_blockboy_logo.width,
                                          image_blockboy_logo.height,
                                          image_blockboy_logo.width * 2,
                                          display_buffer, 0);
                    rg_task_delay(25);
                }

                free(display_buffer);
                free(block_revealed);
            }
            else
            {
                if (display_buffer) free(display_buffer);
                if (block_revealed) free(block_revealed);
            }
        }
        else if (bootanim_mode == 1)  // Scroll animation (DMG style)
        {
            // Create a taller buffer for the scroll area
            int scroll_height = logo_y + image_blockboy_logo.height;
            int buffer_size = image_blockboy_logo.width * scroll_height;
            uint16_t *scroll_buffer = rg_alloc(buffer_size * 2, MEM_FAST);

            if (scroll_buffer)
            {
                int start_y = 0;
                int end_y = logo_y;

                for (int y = start_y; y <= end_y; y += 1)
                {
                    // Fill entire buffer with background
                    for (int i = 0; i < buffer_size; i++)
                        scroll_buffer[i] = DMG_BG;

                    // Draw logo at current y position within buffer
                    for (int py = 0; py < (int)image_blockboy_logo.height; py++)
                    {
                        for (int px = 0; px < (int)image_blockboy_logo.width; px++)
                        {
                            int src_idx = py * image_blockboy_logo.width + px;
                            int dst_idx = (y + py) * image_blockboy_logo.width + px;
                            if (dst_idx < buffer_size)
                                scroll_buffer[dst_idx] = image_blockboy_logo.pixel_data[src_idx];
                        }
                    }

                    // Draw the buffer
                    rg_display_write_rect(logo_x, 0,
                                          image_blockboy_logo.width,
                                          scroll_height,
                                          image_blockboy_logo.width * 2,
                                          scroll_buffer, 0);
                    rg_task_delay(25);
                }

                free(scroll_buffer);
            }
        }

        // Show final logo
        rg_display_write_rect(logo_x, logo_y,
                              image_blockboy_logo.width,
                              image_blockboy_logo.height,
                              image_blockboy_logo.width * 2,
                              (uint16_t*)image_blockboy_logo.pixel_data, 0);

        // Hold logo
        rg_task_delay(800);

        // Fade to black
        rg_display_clear(C_BLACK);
        rg_task_delay(300);
    }
    else
    {
        // Show hourglass if animation disabled or resuming from game
        rg_gui_draw_hourglass();
    }
```

---

## Creating a Logo Bitmap

To create a new logo, use this prompt for ChatGPT:

```
Convert this image to a C header file with RGB565 colors.

Requirements:
- Use EXACTLY this format:
static const struct {
    unsigned int width;
    unsigned int height;
    unsigned int bytes_per_pixel;
    uint16_t pixel_data[WIDTH * HEIGHT];
} image_blockboy_logo = {
    WIDTH, HEIGHT, 2,
    { /* pixel data here */ }
};

- Replace WIDTH and HEIGHT with the actual dimensions
- RGB565 format: 16-bit colors (5 bits red, 6 bits green, 5 bits blue)
- Each pixel as hex value (e.g. 0x7C02)
- Pixels from left to right, top to bottom
- Use 0x7C02 for the green background (#7B8210)
```

---

## DMG Color Reference

| Usage | RGB Hex | RGB565 |
|-------|---------|--------|
| Background (boot) | #7B8210 | 0x7C02 |
| Background (official) | #9BBC0F | 0x9DE1 |
| Light details | #8BAC0F | 0x8D61 |
| Dark details | #306230 | 0x3306 |
| Text (darkest) | #0F380F | 0x09C1 |

---

## Adjusting Timing

**Blocks animation:**
- `BLOCKS_PER_FRAME = 3` - Number of blocks per frame (higher = faster)
- `rg_task_delay(25)` - Delay per frame in ms (lower = faster)

**Scroll animation:**
- `y += 1` - Pixels per frame (higher = faster)
- `rg_task_delay(25)` - Delay per frame in ms (lower = faster)

**Logo display:**
- `rg_task_delay(800)` - How long the logo stays visible
- `rg_task_delay(300)` - Fade to black delay

---

## Checklist for New Version

1. [ ] Copy `image_blockboy_logo.h` to `components/retro-go/bitmaps/`
2. [ ] Add `#include "bitmaps/image_blockboy_logo.h"` to `rg_system.c`
3. [ ] Add `bootanim_mode_t` enum to `gui.h`
4. [ ] Add `int bootanim;` to `retro_gui_t` struct in `gui.h`
5. [ ] Add `SETTING_BOOTANIM` define to `gui.c`
6. [ ] Add setting loading in `gui_init()` in `gui.c`
7. [ ] Add setting saving in `gui_save_config()` in `gui.c`
8. [ ] Add `bootanim_cb()` callback to `main.c`
9. [ ] Add menu option in `options_handler()` in `main.c`
10. [ ] Add boot animation code to `rg_system_init()` in `rg_system.c`

---

## Version Info

- Built for: BlockBoy retro handheld project
- Logo size: 240x58 pixels
- Screen: 240x240 (or similar)
