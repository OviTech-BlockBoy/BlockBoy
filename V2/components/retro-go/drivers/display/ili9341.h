#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <math.h>

#if defined(RG_SCREEN_ROTATE) && RG_SCREEN_ROTATE != 0
#error "RG_SCREEN_ROTATE doesn't do anything on this driver, you have to use the 0x36 command during init!"
#endif

static spi_device_handle_t spi_dev;
static QueueHandle_t spi_transactions;
static QueueHandle_t spi_buffers;

#define SPI_TRANSACTION_COUNT (10)
#define SPI_BUFFER_COUNT      (5)
#define SPI_BUFFER_LENGTH     (LCD_BUFFER_LENGTH * 2)

static inline uint16_t *spi_take_buffer(void)
{
    uint16_t *buffer;
    if (xQueueReceive(spi_buffers, &buffer, pdMS_TO_TICKS(2500)) != pdTRUE)
        RG_PANIC("display");
    return buffer;
}

static inline void spi_give_buffer(uint16_t *buffer)
{
    xQueueSend(spi_buffers, &buffer, portMAX_DELAY);
}

static inline void spi_queue_transaction(const void *data, size_t length, uint32_t type)
{
    if (!data || !length)
        return;

    spi_transaction_t *t;
    xQueueReceive(spi_transactions, &t, portMAX_DELAY);

    *t = (spi_transaction_t){
        .tx_buffer = NULL,
        .length = length * 8, // In bits
        .user = (void *)type,
        .flags = 0,
    };

    if (type & 2)
    {
        t->tx_buffer = data;
    }
    else if (length < 5)
    {
        memcpy(t->tx_data, data, length);
        t->flags = SPI_TRANS_USE_TXDATA;
    }
    else
    {
        t->tx_buffer = memcpy(spi_take_buffer(), data, length);
        t->user = (void *)(type | 2);
    }

    if (spi_device_queue_trans(spi_dev, t, pdMS_TO_TICKS(2500)) != ESP_OK)
    {
        RG_PANIC("display");
    }
}

IRAM_ATTR
static void spi_pre_transfer_cb(spi_transaction_t *t)
{
    // Set the data/command line accordingly
    gpio_set_level(RG_GPIO_LCD_DC, (int)t->user & 1);
}

IRAM_ATTR
static void spi_task(void *arg)
{
    spi_transaction_t *t;

    while (spi_device_get_trans_result(spi_dev, &t, portMAX_DELAY) == ESP_OK)
    {
        if ((int)t->user & 2)
            spi_give_buffer((uint16_t *)t->tx_buffer);
        xQueueSend(spi_transactions, &t, portMAX_DELAY);
    }
}

static void spi_init(void)
{
    spi_transactions = xQueueCreate(SPI_TRANSACTION_COUNT, sizeof(spi_transaction_t *));
    spi_buffers = xQueueCreate(SPI_BUFFER_COUNT, sizeof(uint16_t *));

    while (uxQueueSpacesAvailable(spi_transactions))
    {
        void *trans = malloc(sizeof(spi_transaction_t));
        xQueueSend(spi_transactions, &trans, portMAX_DELAY);
    }

    while (uxQueueSpacesAvailable(spi_buffers))
    {
        void *buffer = rg_alloc(SPI_BUFFER_LENGTH, MEM_DMA);
        xQueueSend(spi_buffers, &buffer, portMAX_DELAY);
    }

    const spi_bus_config_t buscfg = {
        .miso_io_num = RG_GPIO_LCD_MISO,
        .mosi_io_num = RG_GPIO_LCD_MOSI,
        .sclk_io_num = RG_GPIO_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = RG_SCREEN_SPEED,   // Typically SPI_MASTER_FREQ_40M or SPI_MASTER_FREQ_80M
        .mode = 0,                           // SPI mode 0
        .spics_io_num = RG_GPIO_LCD_CS,      // CS pin
        .queue_size = SPI_TRANSACTION_COUNT, // We want to be able to queue 5 transactions at a time
        .pre_cb = &spi_pre_transfer_cb,      // Specify pre-transfer callback to handle D/C line and SPI lock
        .flags = SPI_DEVICE_NO_DUMMY,        // SPI_DEVICE_HALFDUPLEX;
    };

    esp_err_t ret;

    // Initialize the SPI bus
    ret = spi_bus_initialize(RG_SCREEN_HOST, &buscfg, SPI_DMA_CH_AUTO);
    RG_ASSERT(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE, "spi_bus_initialize failed.");

    ret = spi_bus_add_device(RG_SCREEN_HOST, &devcfg, &spi_dev);
    RG_ASSERT(ret == ESP_OK, "spi_bus_add_device failed.");

    rg_task_create("rg_spi", &spi_task, NULL, 1.5 * 1024, RG_TASK_PRIORITY_7, 0); // core 0: parallel met emulatie op core 1
}

static void spi_deinit(void)
{
    // When transactions are still in flight, spi_bus_remove_device fails and spi_bus_free then crashes.
    // The real solution would be to wait for transactions to be done, but this is simpler for now...
    if (spi_bus_remove_device(spi_dev) == ESP_OK)
        spi_bus_free(RG_SCREEN_HOST);
    else
        RG_LOGE("Failed to properly terminate SPI driver!");
}

#define ILI9341_CMD(cmd, data...)                    \
    {                                                \
        const uint8_t c = cmd, x[] = {data};         \
        spi_queue_transaction(&c, 1, 0);             \
        if (sizeof(x))                               \
            spi_queue_transaction(&x, sizeof(x), 1); \
    }

static void lcd_set_vcom(uint8_t value)
{
    // ST7789 0xBB VCOM register, 6-bit value (0..0x3F). Can be changed at runtime
    // without panel reset; takes effect on next refresh.
    ILI9341_CMD(0xBB, value & 0x3F);
}

// Rotation bits (MY|MV|MX|ML|MH) the target's RG_SCREEN_INIT sets up for MADCTL (0x36),
// with the BGR bit excluded -- that bit is controlled live via lcd_set_bgr() instead,
// since it's a per-panel-clone quirk rather than a rotation/orientation setting.
#ifndef RG_SCREEN_MADCTL_ROT
#define RG_SCREEN_MADCTL_ROT 0
#endif
#ifndef ILI9341_MADCTL
#define ILI9341_MADCTL 0x36
#endif
#ifndef ILI9341_MADCTL_BGR
#define ILI9341_MADCTL_BGR 0x08
#endif
#ifndef ILI9341_MADCTL_MY
#define ILI9341_MADCTL_MY 0x80
#endif
#ifndef ILI9341_MADCTL_MX
#define ILI9341_MADCTL_MX 0x40
#endif

// BGR and the 180° flip both live in MADCTL (0x36), so track them together and
// re-send the combined register whenever either changes.
static bool lcd_madctl_bgr = false;
static bool lcd_madctl_flip = false;

static void lcd_write_madctl(void)
{
    // MADCTL (0x36) can be re-sent anytime without a reset; takes effect on next refresh.
    uint8_t val = RG_SCREEN_MADCTL_ROT;
    if (lcd_madctl_flip)
        val ^= (ILI9341_MADCTL_MY | ILI9341_MADCTL_MX); // 180° = toggle row + column order
    if (lcd_madctl_bgr)
        val |= ILI9341_MADCTL_BGR;
    ILI9341_CMD(ILI9341_MADCTL, val);
}

static void lcd_set_bgr(bool enabled)
{
    lcd_madctl_bgr = enabled;
    lcd_write_madctl();
}

static void lcd_set_flip_180(bool enabled)
{
    lcd_madctl_flip = enabled;
    lcd_write_madctl();
}

static void lcd_set_inversion(bool enabled)
{
    // 0x21 = Display Inversion ON, 0x20 = OFF. Some clones default to the opposite state.
    ILI9341_CMD(enabled ? 0x21 : 0x20);
}

// === Software per-channel color correction ===
// Pixels travel through the buffer pool in RGB565, stored byte-swapped (big-endian on wire
// for ST7789). When any channel gain is != 100%, lcd_send_buffer runs them through these
// LUTs before queueing the DMA. LUTs are tiny (128 bytes total), CPU cost ~10-15 cycles/pixel.
static uint8_t r_lut[32];
static uint8_t g_lut[64];
static uint8_t b_lut[32];
static bool color_correction_active = false;

static void rebuild_channel_lut(uint8_t *lut, int size, int gain_pct, int contrast_pct, int gamma_pct)
{
    int max = size - 1;
    int mid = max / 2;
    double gamma = gamma_pct / 100.0;
    for (int i = 0; i < size; i++) {
        double norm = (double)i / (double)max;
        double curved = pow(norm, 1.0 / gamma);
        int v = (int)(curved * max + 0.5);
        v = ((v - mid) * contrast_pct + 50) / 100 + mid;
        v = (v * gain_pct + 50) / 100;
        if (v < 0) v = 0;
        if (v > max) v = max;
        lut[i] = (uint8_t)v;
    }
}

static void lcd_set_color_config(int r_pct, int g_pct, int b_pct, int contrast_pct, int gamma_pct)
{
    rebuild_channel_lut(r_lut, 32, r_pct, contrast_pct, gamma_pct);
    rebuild_channel_lut(g_lut, 64, g_pct, contrast_pct, gamma_pct);
    rebuild_channel_lut(b_lut, 32, b_pct, contrast_pct, gamma_pct);
    color_correction_active = (r_pct != 100) || (g_pct != 100) || (b_pct != 100)
                           || (contrast_pct != 100) || (gamma_pct != 100);
}

static inline void apply_color_correction(uint16_t *buffer, size_t length)
{
    if (!color_correction_active) return;
    for (size_t i = 0; i < length; i++) {
        uint16_t px = buffer[i];
        uint16_t native = (uint16_t)((px << 8) | (px >> 8));
        uint8_t r = (native >> 11) & 0x1F;
        uint8_t g = (native >>  5) & 0x3F;
        uint8_t b =  native        & 0x1F;
        native = (uint16_t)((r_lut[r] << 11) | (g_lut[g] << 5) | b_lut[b]);
        buffer[i] = (uint16_t)((native << 8) | (native >> 8));
    }
}

static void lcd_set_backlight(float percent)
{
    float level = RG_MIN(RG_MAX(percent / 100.f, 0), 1.f);
    int error_code = 0;

#if defined(RG_GPIO_LCD_BCKL)
    // P-Channel MOSFET (AO3401): Inverted logic - LOW=ON, HIGH=OFF
    // 100% brightness = 0% duty cycle (GPIO always LOW)
    // 0% brightness = 100% duty cycle (GPIO always HIGH)
    uint32_t max_duty = (1 << 13);  // 8192 for 13-bit resolution
    uint32_t duty = (uint32_t)(max_duty * (1.0f - level));  // INVERTED for P-channel!

    error_code = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (error_code == ESP_OK)
        error_code = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

#elif defined(RG_TARGET_QTPY_GAMER)
    rg_i2c_gpio_set_direction(AW_TFT_BACKLIGHT, RG_GPIO_ANALOG_OUTPUT);
    rg_i2c_gpio_set_level(AW_TFT_BACKLIGHT, level * 255);
#endif

    if (error_code)
        RG_LOGE("failed setting backlight (0x%02X)\n", error_code);
    else
        RG_LOGI("backlight set to %d%%\n", (int)(100 * level));
}

static void lcd_set_window(int left, int top, int width, int height)
{
    int right = left + width - 1;
    int bottom = top + height - 1;

    if (left < 0 || top < 0 || right >= display.screen.real_width || bottom >= display.screen.real_height)
        RG_LOGW("Bad lcd window (x0=%d, y0=%d, x1=%d, y1=%d)\n", left, top, right, bottom);

    ILI9341_CMD(0x2A, left >> 8, left & 0xff, right >> 8, right & 0xff); // Horiz
    ILI9341_CMD(0x2B, top >> 8, top & 0xff, bottom >> 8, bottom & 0xff); // Vert
    ILI9341_CMD(0x2C);                                                   // Memory write
}

static inline uint16_t *lcd_get_buffer(size_t length)
{
    // RG_ASSERT_ARG(length < LCD_BUFFER_LENGTH);
    return spi_take_buffer();
}

static inline void lcd_send_buffer(uint16_t *buffer, size_t length)
{
    if (length > 0) {
        apply_color_correction(buffer, length);
        spi_queue_transaction(buffer, length * sizeof(*buffer), 3);
    } else {
        spi_give_buffer(buffer);
    }
}

static void lcd_sync(void)
{
    // Unused for SPI LCD
}

static void lcd_init(void)
{
#ifdef RG_GPIO_LCD_BCKL
    // Initialize backlight OFF to avoid the lcd reset flash. NOTE: this panel uses
    // an inverted P-channel MOSFET (LOW=ON, HIGH=OFF), so "off" is MAX duty, not 0.
    // Using duty 0 here would drive the backlight FULLY ON during panel init and
    // show a white flash. rg_display_init() turns it on later, after clearing.
    ledc_timer_config(&(ledc_timer_config_t){
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 5000,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
    });
    ledc_channel_config(&(ledc_channel_config_t){
        .channel = LEDC_CHANNEL_0,
        // OFF for the inverted P-MOSFET (GPIO HIGH). During a showcase transition we
        // keep the retained loading image lit instead (duty 0 = GPIO LOW = ON).
        .duty = display_preserve ? 0 : (1 << 13),
        .gpio_num = RG_GPIO_LCD_BCKL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LEDC_TIMER_0,
    });
#endif

    spi_init();

    // Setup Data/Command line
    gpio_set_direction(RG_GPIO_LCD_DC, GPIO_MODE_OUTPUT);
    gpio_set_level(RG_GPIO_LCD_DC, 1);

    // Showcase transition: the panel still holds its prior config and the loading
    // image in GRAM (a SW reboot doesn't power-cycle it). Skip the reset/init below
    // (which would wipe the image) so it stays on screen for a flash-free switch.
    if (display_preserve)
        return;

#if defined(RG_GPIO_LCD_RST)
    gpio_set_direction(RG_GPIO_LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(RG_GPIO_LCD_RST, 0);
    rg_usleep(100 * 1000);
    gpio_set_level(RG_GPIO_LCD_RST, 1);
    rg_usleep(10 * 1000);
#elif defined(RG_TARGET_QTPY_GAMER)
    rg_i2c_gpio_set_direction(AW_TFT_RESET, RG_GPIO_OUTPUT);
    rg_i2c_gpio_set_level(AW_TFT_RESET, 0);
    rg_usleep(100 * 1000);
    rg_i2c_gpio_set_level(AW_TFT_RESET, 1);
    rg_usleep(10 * 1000);
#endif

    ILI9341_CMD(0x01);          // Reset
    rg_usleep(5 * 1000);        // Wait 5ms after reset
    ILI9341_CMD(0x3A, 0X05);    // Pixel Format Set RGB565
    #ifdef RG_SCREEN_INIT
        RG_SCREEN_INIT();
    #else
        #warning "LCD init sequence is not defined for this device!"
    #endif
    ILI9341_CMD(0x11);  // Exit Sleep
    rg_usleep(10 * 1000);// Wait 10ms after sleep out
    ILI9341_CMD(0x29);  // Display on
}

static void lcd_deinit(void)
{
#ifdef RG_SCREEN_DEINIT
    RG_SCREEN_DEINIT();
#endif
    spi_deinit();
    // gpio_reset_pin(RG_GPIO_LCD_BCKL);
    // gpio_reset_pin(RG_GPIO_LCD_DC);
}

const rg_display_driver_t rg_display_driver_ili9341 = {
    .name = "ili9341",
};
