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

// Clock used for the reset + init sequence, before the bus is handed over to the
// frame pipeline at RG_SCREEN_SPEED. Pixel data at 80MHz sits at the edge of what
// the ST7789 accepts and a bad pixel costs one frame; a single mangled init byte
// leaves the panel black for the entire session, so the ~60 bytes that configure
// the panel are sent slow. Init takes microseconds either way.
#ifndef RG_SCREEN_INIT_SPEED
#define RG_SCREEN_INIT_SPEED (10 * 1000 * 1000)
#endif

// Blind re-runs of reset + init when the module can't be read back (see lcd_init).
#ifndef RG_SCREEN_BLIND_INIT_RETRIES
#define RG_SCREEN_BLIND_INIT_RETRIES (1)
#endif

// Panel supply switch timings (see lcd_power_init). 200ms off is what the manual
// switch test used; the bleeder resistor on the panel rail needs roughly 4 time
// constants to get it down, and there is no reason to cut it fine.
#ifndef RG_LCD_PWR_OFF_US
#define RG_LCD_PWR_OFF_US (200 * 1000)
#endif
#ifndef RG_LCD_PWR_SETTLE_US
#define RG_LCD_PWR_SETTLE_US (50 * 1000)
#endif

// Set while the init sequence runs: the rg_spi task that recycles queued
// transaction descriptors is not started until init is done, so those transfers
// go out synchronously instead of through the queue.
static bool spi_polling_mode = false;
static int spi_clock_hz = 0;

// rg_spi is the only thing that returns transaction descriptors to the pool. If it
// ever exits, every later display write starves -- so its state is tracked and its
// exit is logged instead of being lost.
static volatile bool spi_task_alive = false;

// Defined further down (it needs the transaction pool), used by spi_deinit above it.
static void spi_drain(void);

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

    if (spi_polling_mode)
    {
        // Blocking transfer. Bit 1 of `type` means "buffer came from the pool" and is
        // cleared here: nothing hands it back during init, and we never take one.
        static uint8_t polling_buffer[64] __attribute__((aligned(4)));
        RG_ASSERT(length <= sizeof(polling_buffer), "init transfer too large for polling buffer");
        memcpy(polling_buffer, data, length);
        spi_transaction_t t = {
            .tx_buffer = polling_buffer,
            .length = length * 8, // In bits
            .user = (void *)(uintptr_t)(type & 1),
        };
        if (spi_device_polling_transmit(spi_dev, &t) != ESP_OK)
            RG_LOGE("Init transfer failed\n");
        return;
    }

    // Bounded like the buffer pool above. An unbounded wait here is invisible: the
    // two neighbouring waits (spi_take_buffer, spi_device_queue_trans) both panic
    // with a message after 2.5s, but this one used to be portMAX_DELAY, so if rg_spi
    // ever stops handing descriptors back the caller parks here forever without a
    // single line of output.
    spi_transaction_t *t;
    if (xQueueReceive(spi_transactions, &t, pdMS_TO_TICKS(2500)) != pdTRUE)
    {
        RG_LOGE("SPI transaction pool starved (rg_spi alive=%d)\n", (int)spi_task_alive);
        RG_PANIC("display");
    }

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
    esp_err_t err;

    spi_task_alive = true;

    while ((err = spi_device_get_trans_result(spi_dev, &t, portMAX_DELAY)) == ESP_OK)
    {
        if ((int)t->user & 2)
            spi_give_buffer((uint16_t *)t->tx_buffer);
        xQueueSend(spi_transactions, &t, portMAX_DELAY);
    }

    spi_task_alive = false;
    RG_LOGE("rg_spi exiting: spi_device_get_trans_result returned 0x%X. Display is now stalled!\n", err);
}

static void spi_add_device(int clock_hz)
{
    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = clock_hz,          // Typically SPI_MASTER_FREQ_40M or SPI_MASTER_FREQ_80M
        .mode = 0,                           // SPI mode 0
        .spics_io_num = RG_GPIO_LCD_CS,      // CS pin
        .queue_size = SPI_TRANSACTION_COUNT, // We want to be able to queue 5 transactions at a time
        .pre_cb = &spi_pre_transfer_cb,      // Specify pre-transfer callback to handle D/C line and SPI lock
        .flags = SPI_DEVICE_NO_DUMMY,        // SPI_DEVICE_HALFDUPLEX;
    };

    esp_err_t ret = spi_bus_add_device(RG_SCREEN_HOST, &devcfg, &spi_dev);
    RG_ASSERT(ret == ESP_OK, "spi_bus_add_device failed.");
    spi_clock_hz = clock_hz;
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

    esp_err_t ret;

    // Initialize the SPI bus
    ret = spi_bus_initialize(RG_SCREEN_HOST, &buscfg, SPI_DMA_CH_AUTO);
    RG_ASSERT(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE, "spi_bus_initialize failed.");

    // Comes up at the slow init clock; lcd_init() raises it to RG_SCREEN_SPEED once
    // the panel is configured. No rg_spi task yet -- init runs in polling mode.
    spi_polling_mode = true;
    spi_add_device(RG_SCREEN_INIT_SPEED);
}

// Swaps the device out for one clocked at `clock_hz`. Only legal while nothing is
// in flight and the rg_spi task is not running, i.e. during lcd_init().
static void spi_set_speed(int clock_hz)
{
    if (clock_hz == spi_clock_hz)
        return;
    if (spi_bus_remove_device(spi_dev) != ESP_OK)
    {
        RG_LOGE("Failed to switch SPI clock to %dHz, staying at %dHz\n", clock_hz, spi_clock_hz);
        return;
    }
    spi_add_device(clock_hz);
}

// Hands the bus over to the frame pipeline: full clock, queued transactions again.
static void spi_start_task(void)
{
    spi_set_speed(RG_SCREEN_SPEED);
    spi_polling_mode = false;
    rg_task_create("rg_spi", &spi_task, NULL, 1.5 * 1024, RG_TASK_PRIORITY_7, 1);
}

static void spi_deinit(void)
{
    // Wait for every queued transaction to actually complete first. Without this the
    // teardown logged "Have unfinished transactions" / "Failed to properly terminate
    // SPI driver!" on every app switch and left the peripheral and its DMA chain
    // mid-transfer -- and an app switch here is a RTC_SW_CPU_RST, which does not
    // reset those. The next boot then inherits a half-finished transfer.
    // (This is the "real solution" the old comment here said was left for later.)
    spi_drain();

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

static void lcd_set_vcom_offset(uint8_t value)
{
    // ST7789 0xC5 VCMOFSET, VCMOFS[5:0]: 6-bit value (0x00-0x3F, default 0x20 = 0V),
    // fine-tunes VCOM on top of 0xBB. Used to combat vertical column crosstalk:
    // small steps around 0x20 typically make the vertical "ghost" streaks
    // above/below dark objects disappear.
    ILI9341_CMD(0xC5, value & 0x3F);
}

static void lcd_set_vrh(uint8_t value)
{
    // ST7789 0xC3 VRH Set. Controls positive gamma voltage (column drive strength).
    // Lower VRH = softer column transitions = less vertical crosstalk, but also
    // slightly less contrast. Range 0x00..0x27, default ~0x0F.
    // 0xC2 must have been set to 0x01 during init to enable VRH writes.
    ILI9341_CMD(0xC3, value & 0x3F);
}

static void lcd_set_framerate(uint8_t value)
{
    // ST7789 0xC6 FRCTR2 (RTNA field). 0x0F = 60Hz, 0x00 = 119Hz; lower value =
    // higher refresh. Higher Hz = less tearing, but each line gets less charging
    // time, which can produce horizontal crosstalk (grey smear beside dark blocks).
    ILI9341_CMD(0xC6, value & 0x1F);
}

// === GSCAN (0x45) scanline read test — "software TE-pin" feasibility probe ===
// The module exposes no TE pin and no separate MISO; SDA is bidirectional. This
// bit-bangs a GSCAN read by temporarily detaching CLK/MOSI/CS from the SPI
// peripheral via the GPIO matrix (bus held exclusively), clocking out 0x45 and
// sampling the response on the same data line, then restoring the SPI routing.
// Community quirks handled: first read returns 0, CS must toggle between reads.
// Whether a dummy clock precedes the data differs per datasheet interpretation,
// so both decodes of the raw 17-bit capture are logged.
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <soc/gpio_sig_map.h>

// Blocks until everything we queued has actually gone out on the wire. Taking
// every transaction struct back out of the pool proves none are still in flight;
// they are handed straight back. Needed before grabbing the bus for a bit-banged
// read, and before timing-critical delays in the init sequence -- queueing a
// command is asynchronous, so rg_usleep() right after ILI9341_CMD() would
// otherwise start counting before the panel has even seen the byte.
static void spi_drain(void)
{
    spi_transaction_t *parked[SPI_TRANSACTION_COUNT];
    for (int i = 0; i < SPI_TRANSACTION_COUNT; ++i)
        xQueueReceive(spi_transactions, &parked[i], portMAX_DELAY);
    for (int i = 0; i < SPI_TRANSACTION_COUNT; ++i)
        xQueueSend(spi_transactions, &parked[i], portMAX_DELAY);
}

// Bit-bangs a register read on the shared SDA line: clocks out `cmd`, turns the
// data line around and samples `bits` bits. Same mechanics as lcd_gscan_test()
// below (CLK/MOSI/CS detached from the SPI peripheral through the GPIO matrix,
// bus held exclusively, routing restored afterwards). Returns the raw capture;
// single-parameter reads may or may not be preceded by a dummy clock depending
// on the datasheet reading, so the caller checks both alignments.
static uint32_t lcd_read_reg(uint8_t cmd, int bits)
{
    spi_drain();
    spi_device_acquire_bus(spi_dev, portMAX_DELAY);

    esp_rom_gpio_connect_out_signal(RG_GPIO_LCD_CLK, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(RG_GPIO_LCD_MOSI, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(RG_GPIO_LCD_CS, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction(RG_GPIO_LCD_CLK, GPIO_MODE_OUTPUT);
    gpio_set_direction(RG_GPIO_LCD_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_direction(RG_GPIO_LCD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(RG_GPIO_LCD_CLK, 0); // SPI mode 0: clock idles low
    gpio_set_level(RG_GPIO_LCD_CS, 1);

    gpio_set_level(RG_GPIO_LCD_CS, 0);
    esp_rom_delay_us(1);
    gpio_set_level(RG_GPIO_LCD_DC, 0);
    for (int bit = 7; bit >= 0; bit--)
    {
        gpio_set_level(RG_GPIO_LCD_MOSI, (cmd >> bit) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(RG_GPIO_LCD_CLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(RG_GPIO_LCD_CLK, 0);
    }
    gpio_set_level(RG_GPIO_LCD_DC, 1);
    gpio_set_direction(RG_GPIO_LCD_MOSI, GPIO_MODE_INPUT);
    gpio_pullup_en(RG_GPIO_LCD_MOSI);
    uint32_t raw = 0;
    for (int bit = 0; bit < bits; bit++)
    {
        esp_rom_delay_us(1);
        gpio_set_level(RG_GPIO_LCD_CLK, 1);
        esp_rom_delay_us(1);
        raw = (raw << 1) | gpio_get_level(RG_GPIO_LCD_MOSI);
        gpio_set_level(RG_GPIO_LCD_CLK, 0);
    }
    gpio_pullup_dis(RG_GPIO_LCD_MOSI);
    gpio_set_direction(RG_GPIO_LCD_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_level(RG_GPIO_LCD_CS, 1);

    // Restore SPI peripheral routing (SPI2 = FSPI on the ESP32-S3)
    esp_rom_gpio_connect_out_signal(RG_GPIO_LCD_CLK, FSPICLK_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(RG_GPIO_LCD_MOSI, FSPID_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(RG_GPIO_LCD_CS, FSPICS0_OUT_IDX, false, false);

    spi_device_release_bus(spi_dev);
    return raw;
}

// True when the panel answers register reads with a value we know we just wrote:
// RDCOLMOD (0x0C) must read back 0x05 (RGB565). If this fails the module simply
// cannot be read on this board and lcd_panel_is_on() must not be trusted -- we
// then fall back to the old fire-and-forget behaviour instead of retrying blindly.
static bool lcd_readback_works(void)
{
    lcd_read_reg(0x0C, 9); // known quirk: the first read after CS activity returns 0
    uint32_t v = lcd_read_reg(0x0C, 9);
    bool ok = (((v >> 1) & 0xFF) == 0x05) || ((v & 0xFF) == 0x05);
    RG_LOGI("Panel readback %s (RDCOLMOD raw9=0x%03X, expect 0x05)\n", ok ? "OK" : "UNAVAILABLE", (unsigned)v);
    return ok;
}

// RDDPM (0x0A) Read Display Power Mode: D7 booster on, D4 sleep out, D2 display on.
// All three must be set for the panel to actually drive the glass. This is what
// distinguishes "init landed" from "panel is sitting there ignoring us".
static bool lcd_panel_is_on(void)
{
    uint32_t v = lcd_read_reg(0x0A, 9);
    uint8_t nodummy = (v >> 1) & 0xFF, dummy = v & 0xFF;
    bool on = ((nodummy & 0x94) == 0x94) || ((dummy & 0x94) == 0x94);
    RG_LOGI("Panel power mode: raw9=0x%03X nodummy=0x%02X dummy=0x%02X -> %s\n",
            (unsigned)v, nodummy, dummy, on ? "booster+sleepout+display ON" : "NOT ON");
    return on;
}

static void lcd_gscan_test(void)
{
    // NOTE: the IDF SPI master only supports portMAX_DELAY here. Safe because
    // rg_display_gscan_test() blocks frame submission before calling us, so the
    // transaction queue is guaranteed to drain.
    spi_device_acquire_bus(spi_dev, portMAX_DELAY);

    // Detach pins from the SPI peripheral; drive them as plain GPIOs.
    esp_rom_gpio_connect_out_signal(RG_GPIO_LCD_CLK, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(RG_GPIO_LCD_MOSI, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(RG_GPIO_LCD_CS, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction(RG_GPIO_LCD_CLK, GPIO_MODE_OUTPUT);
    gpio_set_direction(RG_GPIO_LCD_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_direction(RG_GPIO_LCD_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(RG_GPIO_LCD_CLK, 0); // SPI mode 0: clock idles low
    gpio_set_level(RG_GPIO_LCD_CS, 1);

    RG_LOGI("GSCAN test: 64 reads, ~1ms apart (~4 frames at 60Hz)...\n");

    int nonzero = 0, changing = 0;
    uint32_t prev_raw = 0;

    // Reads a register: clocks out `cmd`, turns the shared SDA line around and
    // clocks in `bits` bits. All-ones = nobody drove the line (no response).
    #define GSCAN_READ_REG(cmd, bits, raw_out)                        \
    {                                                                 \
        gpio_set_level(RG_GPIO_LCD_CS, 0);                            \
        esp_rom_delay_us(1);                                          \
        gpio_set_level(RG_GPIO_LCD_DC, 0);                            \
        for (int bit = 7; bit >= 0; bit--)                            \
        {                                                             \
            gpio_set_level(RG_GPIO_LCD_MOSI, ((cmd) >> bit) & 1);     \
            esp_rom_delay_us(1);                                      \
            gpio_set_level(RG_GPIO_LCD_CLK, 1);                       \
            esp_rom_delay_us(1);                                      \
            gpio_set_level(RG_GPIO_LCD_CLK, 0);                       \
        }                                                             \
        gpio_set_level(RG_GPIO_LCD_DC, 1);                            \
        gpio_set_direction(RG_GPIO_LCD_MOSI, GPIO_MODE_INPUT);        \
        gpio_pullup_en(RG_GPIO_LCD_MOSI);                             \
        uint64_t raw = 0;                                             \
        for (int bit = 0; bit < (bits); bit++)                        \
        {                                                             \
            esp_rom_delay_us(1);                                      \
            gpio_set_level(RG_GPIO_LCD_CLK, 1);                       \
            esp_rom_delay_us(1);                                      \
            raw = (raw << 1) | gpio_get_level(RG_GPIO_LCD_MOSI);      \
            gpio_set_level(RG_GPIO_LCD_CLK, 0);                       \
        }                                                             \
        gpio_pullup_dis(RG_GPIO_LCD_MOSI);                            \
        gpio_set_direction(RG_GPIO_LCD_MOSI, GPIO_MODE_OUTPUT);       \
        gpio_set_level(RG_GPIO_LCD_CS, 1);                            \
        raw_out = raw;                                                \
    }

    // Sanity checks with KNOWN expected values: RDMADCTL (0x0B) must return 0x60
    // (we wrote it during init) and RDCOLMOD (0x0C) must return 0x05 (RGB565).
    // If these read back correctly the SDA read path works; all-zeros/ones means
    // the module can't be read at all and any scanline-based sync is off the table.
    // Single-parameter reads have no dummy clock; capture 9 bits to check both
    // alignments. RDDID (0x04) is logged too, but its value is chip-dependent.
    for (int i = 0; i < 2; i++)
    {
        uint64_t v = 0;
        GSCAN_READ_REG(0x0B, 9, v);
        RG_LOGI("RDMADCTL[%d] raw9=0x%03X nodummy=0x%02X dummy=0x%02X (expect 0x60)\n",
                i, (unsigned)v, (unsigned)((v >> 1) & 0xFF), (unsigned)(v & 0xFF));
        GSCAN_READ_REG(0x0C, 9, v);
        RG_LOGI("RDCOLMOD[%d] raw9=0x%03X nodummy=0x%02X dummy=0x%02X (expect 0x05)\n",
                i, (unsigned)v, (unsigned)((v >> 1) & 0xFF), (unsigned)(v & 0xFF));
        GSCAN_READ_REG(0x04, 25, v);
        RG_LOGI("RDDID[%d] id24=0x%06X\n", i, (unsigned)(v & 0xFFFFFF));
    }

    for (int i = 0; i < 64; i++)
    {
        uint64_t raw = 0;
        GSCAN_READ_REG(0x45, 17, raw); // 1 possible dummy + 16 data bits

        uint32_t v_dummy = raw & 0xFFFF;          // decode assuming leading dummy clock
        uint32_t v_nodummy = (raw >> 1) & 0xFFFF; // decode assuming no dummy clock
        RG_LOGI("GSCAN[%02d] raw=0x%05X dummy=%u/0x%04X nodummy=%u/0x%04X\n",
                i, (unsigned)raw,
                (unsigned)(v_dummy & 0x3FF), (unsigned)v_dummy,
                (unsigned)(v_nodummy & 0x3FF), (unsigned)v_nodummy);

        if (raw != 0 && raw != 0x1FFFF) nonzero++;
        if (i > 0 && raw != prev_raw) changing++;
        prev_raw = raw;

        esp_rom_delay_us(1000);
    }
    #undef GSCAN_READ_REG

    // Restore SPI peripheral routing (SPI2 = FSPI on the ESP32-S3)
    gpio_set_direction(RG_GPIO_LCD_CLK, GPIO_MODE_OUTPUT);
    gpio_set_direction(RG_GPIO_LCD_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_direction(RG_GPIO_LCD_CS, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(RG_GPIO_LCD_CLK, FSPICLK_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(RG_GPIO_LCD_MOSI, FSPID_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(RG_GPIO_LCD_CS, FSPICS0_OUT_IDX, false, false);

    spi_device_release_bus(spi_dev);

    RG_LOGI("GSCAN result: %d/64 reads nonzero, %d changed between reads\n", nonzero, changing);
    if (nonzero == 0)
        RG_LOGI("GSCAN verdict: all zeros/ones - panel does not answer, software-TE NOT possible\n");
    else if (changing > 8)
        RG_LOGI("GSCAN verdict: live scanline readable - software-TE looks POSSIBLE on this panel!\n");
    else
        RG_LOGI("GSCAN verdict: reads answer but don't advance - inconclusive\n");
}

// Reinitialise the display at runtime when the user switches screen type.
// Does a software reset followed by the full init for the selected panel, then
// exits sleep and turns the display on.  Safe to call while the SPI bus is idle
// (i.e. from a menu callback with no ongoing DMA transfers).
//   type 0 = ST7789V3  (HSD028B3N3-P 2.8" IPS — V3 screen)
//   type 1 = ILI9341   (V2 screen)
static void lcd_reinit_display(int type)
{
    // spi_drain() after each timed command: queueing is asynchronous, so without it
    // the delay starts before the panel has even seen the byte (see lcd_send_init_sequence).
    ILI9341_CMD(0x01);        // Software Reset
    spi_drain();
    rg_usleep(120 * 1000);    // Datasheet: up to 120ms before next command

    if (type == 0) {
        // ST7789P3 — factory INI order: Sleep Out BEFORE register writes.
        // Verbatim from V3 config.h RG_SCREEN_INIT (= manufacturer INI, MADCTL 0x60).
        ILI9341_CMD(0x11);        // Sleep Out
        spi_drain();
        rg_usleep(120 * 1000);
        ILI9341_CMD(0x3A, 0x05);  // Pixel Format: RGB565
        ILI9341_CMD(0x36, 0x60);
        ILI9341_CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33);
        ILI9341_CMD(0xB7, 0x42);
        ILI9341_CMD(0xBB, 0x34);
        ILI9341_CMD(0xC0, 0x2C);
        ILI9341_CMD(0xC2, 0x01);
        ILI9341_CMD(0xC3, 0x0F);
        ILI9341_CMD(0xC4, 0x20);
        ILI9341_CMD(0xC6, 0x02);  // FRCTR2 ~105Hz (user preference; manufacturer 0x0F=60Hz tears worse)
        ILI9341_CMD(0xD0, 0xA7, 0xA1);
        ILI9341_CMD(0xD0, 0xA4, 0xA1);
        ILI9341_CMD(0xD6, 0xA1);
        ILI9341_CMD(0xE0, 0xF0, 0x04, 0x0A, 0x09, 0x08, 0x25, 0x25, 0x33, 0x3C, 0x37, 0x14, 0x14, 0x29, 0x2F);
        ILI9341_CMD(0xE1, 0xF0, 0x05, 0x08, 0x07, 0x06, 0x02, 0x25, 0x32, 0x3B, 0x38, 0x12, 0x12, 0x27, 0x31);
        ILI9341_CMD(0x21);  // Display Inversion ON (IPS)
        ILI9341_CMD(0x29);  // Display ON
        return;
    } else {
        ILI9341_CMD(0x3A, 0x05);  // Pixel Format: RGB565
        // ILI9341 — verbatim from V2 config.h RG_SCREEN_INIT
        ILI9341_CMD(0xCF, 0x00, 0xc3, 0x30);
        ILI9341_CMD(0xED, 0x64, 0x03, 0x12, 0x81);
        ILI9341_CMD(0xE8, 0x85, 0x00, 0x78);
        ILI9341_CMD(0xCB, 0x39, 0x2c, 0x00, 0x34, 0x02);
        ILI9341_CMD(0xF7, 0x20);
        ILI9341_CMD(0xEA, 0x00, 0x00);
        ILI9341_CMD(0xC0, 0x1B);
        ILI9341_CMD(0xC1, 0x12);
        ILI9341_CMD(0xC5, 0x32, 0x3C);
        ILI9341_CMD(0xC7, 0x91);
        ILI9341_CMD(0x36, 0xA8);  // MY|MV|BGR
        ILI9341_CMD(0xB1, 0x00, 0x10);
        ILI9341_CMD(0xB6, 0x0A, 0xA2);
        ILI9341_CMD(0xF6, 0x01, 0x30);
        ILI9341_CMD(0xF2, 0x00);
        ILI9341_CMD(0xE0, 0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43, 0x47, 0x09, 0x15, 0x12, 0x16, 0x19);
        ILI9341_CMD(0xE1, 0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44, 0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19);
    }

    ILI9341_CMD(0x11);        // Sleep Out
    spi_drain();
    rg_usleep(120 * 1000);    // Datasheet: 120ms for supply voltages/clocks to stabilize
    ILI9341_CMD(0x29);        // Display ON
    spi_drain();
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

// Hardware reset over RESX. Uses gpio_config() rather than gpio_set_direction():
// on the ESP32-S3 the reset pin is GPIO3, which is RTC-capable *and* a strapping
// pin (JTAG_SEL). gpio_set_direction() touches neither the RTC_IO mux nor the
// IO_MUX function select, so if the pad is ever left under RTC control the level
// changes are silently swallowed and the panel never actually gets reset -- and
// because the RTC domain survives a software reset, it stays that way until the
// power is physically removed. gpio_config() calls rtc_gpio_deinit() and forces
// PIN_FUNC_GPIO, which is exactly what this pin needs. See gpio_config() in
// esp_driver_gpio/src/gpio.c.
static void lcd_hw_reset(void)
{
#if defined(RG_GPIO_LCD_RST)
    gpio_reset_pin(RG_GPIO_LCD_RST);
    gpio_config(&(gpio_config_t){
        .pin_bit_mask = 1ULL << RG_GPIO_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    });
    // Park HIGH first so the panel sees a clean falling edge even when the pin
    // was already low (e.g. straight out of the boot ROM, where it floats).
    gpio_set_level(RG_GPIO_LCD_RST, 1);
    rg_usleep(20 * 1000);
    gpio_set_level(RG_GPIO_LCD_RST, 0);
    rg_usleep(120 * 1000);  // Datasheet needs >10us; we hold far longer on purpose
    gpio_set_level(RG_GPIO_LCD_RST, 1);
    rg_usleep(150 * 1000);  // Factory INI: 120ms after HW reset, with margin
#elif defined(RG_TARGET_QTPY_GAMER)
    rg_i2c_gpio_set_direction(AW_TFT_RESET, RG_GPIO_OUTPUT);
    rg_i2c_gpio_set_level(AW_TFT_RESET, 0);
    rg_usleep(100 * 1000);
    rg_i2c_gpio_set_level(AW_TFT_RESET, 1);
    rg_usleep(10 * 1000);
#endif
}

static void lcd_send_init_sequence(void)
{
    // Factory INI order (HSD028B3N3-P / ST7789P3): Sleep Out (charge pumps on)
    // BEFORE any register writes, with the datasheet 120ms stabilization delays.
    // spi_drain() after each timed command: queueing is asynchronous, so without
    // it the delays would start before the panel has seen the byte.
    ILI9341_CMD(0x01);          // Software Reset (harmless after HW reset; covers boards without RST)
    spi_drain();
    rg_usleep(120 * 1000);      // Datasheet: up to 120ms before next command
    ILI9341_CMD(0x11);          // Sleep Out
    spi_drain();
    rg_usleep(120 * 1000);      // Datasheet: 120ms for supply voltages/clocks to stabilize
    ILI9341_CMD(0x3A, 0X05);    // Pixel Format Set RGB565
    #ifdef RG_SCREEN_INIT
        RG_SCREEN_INIT();
    #else
        #warning "LCD init sequence is not defined for this device!"
    #endif
    ILI9341_CMD(0x29);  // Display on
    spi_drain();
}

#if defined(RG_GPIO_LCD_PWR)
// Panel supply switch: P-channel MOSFET on RG_GPIO_LCD_PWR, LOW = on (same inverted
// logic as the backlight MOSFET).
//
// Why this exists: the ST7789P3 sometimes latches an internal power-on state that RESX
// cannot clear. Measured on hardware, with the board left powered throughout: pulling
// the panel's FPC and reconnecting it recovers a black screen, while a RESX pulse plus
// a full re-init does not. Only removing the panel's own supply works. So every cold
// start now begins with a firmware-timed power cycle, with the rest of the board
// already settled instead of everything ramping at once.
//
// The data lines are parked low for the duration. An output left high feeds the panel
// through its ESD clamp diodes and holds its rail near 2.6V -- right at the datasheet's
// 2.65V minimum for VCC, so the internal POR never sees a real power-down and the whole
// exercise achieves nothing.
static void lcd_power_init(bool cycle)
{
    uint64_t pins = 1ULL << RG_GPIO_LCD_PWR;
    if (cycle)
    {
        pins |= (1ULL << RG_GPIO_LCD_DC) | (1ULL << RG_GPIO_LCD_CS)
              | (1ULL << RG_GPIO_LCD_CLK) | (1ULL << RG_GPIO_LCD_MOSI);
    #if defined(RG_GPIO_LCD_RST)
        pins |= 1ULL << RG_GPIO_LCD_RST;
    #endif
    }

    gpio_config(&(gpio_config_t){
        .pin_bit_mask = pins,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    });

    // Showcase transition: the panel is holding the loading image from before the
    // reboot. Cutting its supply would wipe it, which is the one thing that flow
    // exists to avoid. Just make sure the switch is driven on instead of floating.
    if (!cycle)
    {
        gpio_set_level(RG_GPIO_LCD_PWR, 0);
        return;
    }

    gpio_set_level(RG_GPIO_LCD_DC, 0);
    gpio_set_level(RG_GPIO_LCD_CS, 0);
    gpio_set_level(RG_GPIO_LCD_CLK, 0);
    gpio_set_level(RG_GPIO_LCD_MOSI, 0);
#if defined(RG_GPIO_LCD_RST)
    gpio_set_level(RG_GPIO_LCD_RST, 0);
#endif

    gpio_set_level(RG_GPIO_LCD_PWR, 1); // off
    rg_usleep(RG_LCD_PWR_OFF_US);
    gpio_set_level(RG_GPIO_LCD_PWR, 0); // on
    rg_usleep(RG_LCD_PWR_SETTLE_US);

    RG_LOGI("Panel power cycled (%dms off, %dms settle)\n",
            RG_LCD_PWR_OFF_US / 1000, RG_LCD_PWR_SETTLE_US / 1000);
}
#endif

// The user-tuned panel drive registers. Called from lcd_init() while the bus is still
// at RG_SCREEN_INIT_SPEED and the transfers are synchronous. These four control the
// panel's analog drive (VCOM, VCOM offset, VRH, frame rate); if one of them lands
// wrong the panel stays powered and keeps accepting pixels at full frame rate while
// the glass shows nothing -- the console looks alive over serial and dead in the hand.
// They used to be written from rg_display_init() *after* lcd_init() had already
// switched the bus to RG_SCREEN_SPEED, which is the one part of panel configuration
// that was still going out at 80MHz.
static void lcd_apply_panel_tuning(void)
{
    lcd_set_vcom(display_init_vcom);
    lcd_set_vcom_offset(display_init_vcom_offset);
    lcd_set_vrh(display_init_vrh);
    lcd_set_framerate(display_init_framerate);
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

#if defined(RG_GPIO_LCD_PWR)
    // Before anything touches the bus: give the panel a clean supply of its own.
    lcd_power_init(!display_preserve);
#endif

    spi_init();

    // Setup Data/Command line. gpio_config() rather than gpio_set_direction() for
    // the same reason as in lcd_hw_reset(): it is the only call that guarantees the
    // pad is a plain GPIO output (PIN_FUNC_GPIO, out of RTC mode, no stray pulls).
    gpio_config(&(gpio_config_t){
        .pin_bit_mask = 1ULL << RG_GPIO_LCD_DC,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    });
    gpio_set_level(RG_GPIO_LCD_DC, 1);

    // Showcase transition: the panel still holds its prior config and the loading
    // image in GRAM (a SW reboot doesn't power-cycle it). Skip the reset/init below
    // (which would wipe the image) so it stays on screen for a flash-free switch.
    if (display_preserve)
    {
        lcd_apply_panel_tuning();
        spi_start_task();
        return;
    }

    // Let the panel's own power-on reset finish before we talk to it. We get here a
    // few hundred ms after the rail comes up, and the 3V3 rail has little margin on
    // this board: a module whose internal POR threshold sits at the high end of the
    // batch spread can still be holding itself in reset when the first RESX pulse
    // and init sequence go past, and then ignores the lot -- backlight lit, glass
    // black, for the whole session. Costs 100ms of boot time on every unit.
    rg_usleep(100 * 1000);

    // Reset + init, verified. A panel that came out of a marginal power-up can sit
    // there ignoring the whole sequence: the backlight is lit but the glass stays
    // black, and nothing on screen ever appears again for that session. Read the
    // power mode back and redo the reset when it did not take.
    //
    // When the module does not answer register reads there is nothing to check, so
    // instead of sending the sequence once and hoping, we blindly repeat the whole
    // reset + init RG_SCREEN_BLIND_INIT_RETRIES extra times. That is exactly what
    // the user's power cycle does by hand, and it is the only recovery available
    // without readback. ~550ms per extra pass, once per boot.
    bool verify = false, panel_on = false;
    int attempt = 1, max_attempts = 3;
    for (;; ++attempt)
    {
        lcd_hw_reset();
        lcd_send_init_sequence();

        if (attempt == 1)
        {
            verify = lcd_readback_works();
            if (!verify)
                max_attempts = 1 + RG_SCREEN_BLIND_INIT_RETRIES;
        }

        if (verify)
        {
            if ((panel_on = lcd_panel_is_on()))
                break;
            RG_LOGE("Panel did not come up (attempt %d/%d), resetting and retrying...\n", attempt, max_attempts);
        }

        if (attempt >= max_attempts)
            break;
    }

    if (!verify)
        RG_LOGI("Panel init sent %d times (no readback on this module, so unverifiable)\n", attempt);
    else if (!panel_on)
        RG_LOGE("Panel never came up after %d attempts!\n", attempt);
    else if (attempt > 1)
        RG_LOGW("Panel init took %d attempts\n", attempt);

    // Non-default panel type: replace the sequence we just sent with the right one,
    // still at the slow init clock (this is why it happens here and not in
    // rg_display_init() after lcd_init() returns).
    if (display_init_type != 0)
        lcd_reinit_display(display_init_type);

    lcd_apply_panel_tuning();
    spi_start_task();
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
