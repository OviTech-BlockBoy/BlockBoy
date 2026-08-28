// Target definition
#define RG_TARGET_NAME             "0v1Tech-BlockBoy"

// 0v1Tech BlockBoy branding
#define RG_PROJECT_NAME            "BlockBoy"
#define RG_PROJECT_VER             "V3-3.1.0"
#define RG_PROJECT_WEBSITE         "http://blockboy.nl"
#define RG_PROJECT_CREDITS         "BlockBoy by 0v1Tech\nBased on Retro-Go by ducalex\n"

// --- OTA updates -----------------------------------------------------------
// The customer pulls updates through the menu (Settings > Firmware update).
// The launcher downloads to the SD card, the 'flasher' app writes to flash.
#define RG_UPDATER_ENABLE 1

// Unique per device model. RG_TARGET_NAME can't be used for this: V2 and V3
// share that name while having a different display and a different pinout.
// A V3 package on a V2 = a bricked device.
#define RG_OTA_MODEL "blockboy-v3-n16r8"

// Hardware marker. Flash size identifies V1 (4MB) but says nothing about V2 vs
// V3 -- both are N16R8. Production burns this magic into the first word of
// eFuse BLOCK_KEY0 on every V3 (see efuse-tool/). An empty block reads 0, so
// every V2 already in the field identifies itself correctly without being
// touched. eFuse bits only go 0 -> 1, so the marker survives erase_flash.
#define RG_HW_MARKER_MAGIC 0x0B10CB03

// Semver, purely for comparing versions. RG_PROJECT_VER above is the text the
// user sees; it can't be sorted reliably.
// Bump on EVERY release you publish.
#define RG_FIRMWARE_VERSION "3.1.0"

// Bump whenever the partition layout in rg_config.py changes. Devices then
// refuse packages with a different layout -- those would land at the wrong
// offsets. Such devices have to be updated through the web flasher.
#define RG_LAYOUT_VERSION 1

// Manifest in the firmware repo. Small file, so fine in git; the binaries
// themselves hang as assets under a GitHub Release.
#define RG_OTA_MANIFEST_URL \
    "https://raw.githubusercontent.com/OviTech-BlockBoy/BlockBoy/main/ota/v3-n16r8.json"

// Storage
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDSPI_HOST       SPI3_HOST
#define RG_STORAGE_SDSPI_SPEED      SDMMC_FREQ_DEFAULT  // 20MHz (most stable)

// Audio
#define RG_AUDIO_USE_INT_DAC        0   // 0 = Disable, 1 = GPIO25, 2 = GPIO26, 3 = Both
#define RG_AUDIO_USE_EXT_DAC        1   // 0 = Disable, 1 = Enable
#define RG_AUDIO_USE_UAC_HOST       1   // 0 = Disable, 1 = Enable (USB-C DAC via USB OTG)

// Video - ST7789V3 (HSD028B3N3-P 2.8" IPS, 240x320 native portrait used as landscape)
#define RG_SCREEN_DRIVER            0   // 0 = ILI9341 driver (also drives ST7789 via same SPI/ILI9341_CMD path)
#define RG_SCREEN_HOST              SPI2_HOST
#define RG_SCREEN_SPEED             SPI_MASTER_FREQ_80M
#define RG_SCREEN_BACKLIGHT         1
#define RG_SCREEN_WIDTH             320
#define RG_SCREEN_HEIGHT            240
#define RG_SCREEN_ROTATE            0
// Lens crop / panel offset for the V3 ST7789 panel. Tuned on-device via the
// "Screen position" menu. Keeps all drawing inside the lens opening so the
// bright panel edges don't shine past the bezel. Visible area becomes 288x238.
#define RG_SCREEN_VISIBLE_AREA      {24, 0, 8, 2} // Left, Top, Right, Bottom
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0} // Left, Top, Right, Bottom

// ST7789P3 init sequence — exact match of the manufacturer INI
// "ST7789V3(P3)-HSD2.8IPS(HSD028B3N3-P)-2.2Gamma(2).INI", with MADCTL as the
// only deviation: 0x60 (MX|MV) for hardware landscape instead of 0x00.
// Datasheet KLD2877HQIA rev 2.0 (2026-04-09): panel IC changed from ST7789V3
// to ST7789P3 — retune VCOM/framerate via the Color tuning menu on the actual
// panel batch, don't reuse values tuned on V3 silicon.
// The 14-pin module has no TE pin, so tearing can only be masked (framerate
// trade-off), never synced away.
#define RG_SCREEN_INIT()                                                                                           \
    ILI9341_CMD(0x36, 0x60);                              /* MADCTL: hardware landscape (MX|MV) */                 \
    ILI9341_CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33);      /* PORCTRL (manufacturer values) */                      \
    ILI9341_CMD(0xB7, 0x42);                              /* Gate Control */                                       \
    ILI9341_CMD(0xBB, 0x34);                              /* VCOMS = 1.425V (manufacturer) */                      \
    ILI9341_CMD(0xC0, 0x2C);                              /* LCMCTRL */                                            \
    ILI9341_CMD(0xC2, 0x01);                              /* VDVVRHEN */                                           \
    ILI9341_CMD(0xC3, 0x0F);                              /* VRHS */                                               \
    ILI9341_CMD(0xC4, 0x20);                              /* VDVS */                                               \
    ILI9341_CMD(0xC6, 0x02);                              /* FRCTR2 ~105Hz. Manufacturer value is 0x0F (60Hz) but its tearing was judged worse than the slight brightness/color cost (verified on P3 panel, 2026-07). Tunable via Color tuning > Framerate. */ \
    ILI9341_CMD(0xD0, 0xA7, 0xA1);                        /* PWCTRL1 cmd2 page enable variant */                   \
    ILI9341_CMD(0xD0, 0xA4, 0xA1);                        /* PWCTRL1 actual */                                     \
    ILI9341_CMD(0xD6, 0xA1);                              /* Panel power-mode tweak */                             \
    ILI9341_CMD(0xE0, 0xF0, 0x04, 0x0A, 0x09, 0x08, 0x25, 0x25, 0x33, 0x3C, 0x37, 0x14, 0x14, 0x29, 0x2F);         \
    ILI9341_CMD(0xE1, 0xF0, 0x05, 0x08, 0x07, 0x06, 0x02, 0x25, 0x32, 0x3B, 0x38, 0x12, 0x12, 0x27, 0x31);         \
    ILI9341_CMD(0x21);                                    /* Display Inversion ON (required for IPS) */


// Input
// V3 layout - D-pad + A/B/SELECT/START on direct GPIOs, L/R/X/Y + MENU/OPTION via ADC rotary wheels.
// GPIO 7 and 4 are intentionally free for the two DRV5055 Hall sensors of the older
// board revision. Current boards use a TMAG5273 on I2C instead; the firmware detects
// which is fitted and falls back to the DRV5055 path when no TMAG answers.
#define RG_GAMEPAD_GPIO_MAP {\
    {RG_KEY_UP,     .num = GPIO_NUM_21, .pullup = 1, .level = 0},\
    {RG_KEY_RIGHT,  .num = GPIO_NUM_46, .pullup = 1, .level = 0},\
    {RG_KEY_DOWN,   .num = GPIO_NUM_18, .pullup = 1, .level = 0},\
    {RG_KEY_LEFT,   .num = GPIO_NUM_45, .pullup = 1, .level = 0},\
    {RG_KEY_SELECT, .num = GPIO_NUM_16, .pullup = 1, .level = 0},\
    {RG_KEY_START,  .num = GPIO_NUM_17, .pullup = 1, .level = 0},\
    {RG_KEY_A,      .num = GPIO_NUM_15, .pullup = 1, .level = 0},\
    {RG_KEY_B,      .num = GPIO_NUM_6,  .pullup = 1, .level = 0},\
}

// ADC rotary wheels (SLLB120300) - resistor ladder, 3 positions per wheel.
// Brightness wheel: GPIO 1 / ADC1_CH0 - rotate X/Y + push (OPTION)
// Volume wheel:     GPIO 2 / ADC1_CH1 - rotate L/R + push (MENU). L/R swapped vs voltage direction so up = volume up.
#define RG_GAMEPAD_ADC_MAP {\
    {RG_KEY_X,      .unit = ADC_UNIT_1, .channel = ADC_CHANNEL_0, .atten = ADC_ATTEN_DB_11, .min =  300, .max = 1000},\
    {RG_KEY_Y,      .unit = ADC_UNIT_1, .channel = ADC_CHANNEL_0, .atten = ADC_ATTEN_DB_11, .min = 1001, .max = 2200},\
    {RG_KEY_OPTION, .unit = ADC_UNIT_1, .channel = ADC_CHANNEL_0, .atten = ADC_ATTEN_DB_11, .min =    0, .max =  250},\
    {RG_KEY_R,      .unit = ADC_UNIT_1, .channel = ADC_CHANNEL_1, .atten = ADC_ATTEN_DB_11, .min =  300, .max = 1000},\
    {RG_KEY_L,      .unit = ADC_UNIT_1, .channel = ADC_CHANNEL_1, .atten = ADC_ATTEN_DB_11, .min = 1001, .max = 2200},\
    {RG_KEY_MENU,   .unit = ADC_UNIT_1, .channel = ADC_CHANNEL_1, .atten = ADC_ATTEN_DB_11, .min =    0, .max =  250},\
}

// Virtual button combinations. OPTION deliberately has none: on this board it
// sits on the brightness wheel push, so a SELECT+A shortcut on top of that only
// gets in the way. V1 and V2 keep theirs -- they have no OPTION button at all,
// and the combination is the only way to reach that menu there.
#define RG_GAMEPAD_VIRT_MAP {\
    {RG_KEY_MENU,   .src = RG_KEY_START | RG_KEY_SELECT},\
}

// Deep-sleep wake: pressing any of these buttons wakes the device. Only
// GPIO0-21 are RTC-capable on the ESP32-S3, so LEFT(45)/RIGHT(46) can't be
// used here. Buttons are active-low. A/B/SELECT/START/UP/DOWN cover it.
#define RG_GPIO_WAKE_MASK  ((1ULL << GPIO_NUM_15) | (1ULL << GPIO_NUM_6)  | \
                            (1ULL << GPIO_NUM_16) | (1ULL << GPIO_NUM_17) | \
                            (1ULL << GPIO_NUM_18) | (1ULL << GPIO_NUM_21))

// RGB status LED (NH-B1515RGBA-GF, common-anode on B+). NOTE: the EasyEDA
// footprint has its pad numbers on the wrong corners; on this board the LED is
// hand-rotated 180 degrees which nets out to the intended mapping (verified
// on-device 2026-07): IO38 = BLUE, IO8 = GREEN, RED = MCP73871 STAT (charge).
// Fix the footprint pad numbering for the next PCB revision.
//
// Status LED (S-LED): IO8 via N-FET gate (proto #2: FET replaces D2, 100k
// gate pulldown; HIGH = on, leak-free). ACTIVE-HIGH. Dimmed with LEDC PWM
// proportionally to battery level.
// NOTE: do NOT flash ON_LEVEL=1 firmware on a board that still has D1/D2
// fitted -- the idle/sleep park level (LOW) would light its LEDs permanently.
#define RG_GPIO_LED                 GPIO_NUM_8
#define RG_GPIO_LED_ON_LEVEL        1

// Second LED channel (S-LED1): BLUE die of the common-anode RGB LED, driven via
// R22 and the second half of Q5 on IO38. ACTIVE-HIGH, same as IO8: the pin feeds
// a FET gate, so HIGH switches the die on. Exposed as a manual on/off test toggle
// in the LED options menu.
// This was defined as ACTIVE-LOW until 2026-07-30. That inverted the menu, but
// worse: the idle/sleep park level is !ON_LEVEL, so every boot actively drove
// this pin HIGH and gpio_hold_en() held it there -- the blue die sat permanently
// lit (faintly, and independent of USB/charger, which made it look like a
// hardware leak). Verified on-device: menu "On" turned the LED off.
#define RG_GPIO_LED2                GPIO_NUM_38
#define RG_GPIO_LED2_ON_LEVEL       1

// Battery
#define RG_BATTERY_DRIVER           1   // Enabled - battery monitoring active
#define RG_BATTERY_ADC_UNIT         ADC_UNIT_1
#define RG_BATTERY_ADC_CHANNEL      ADC_CHANNEL_4  // GPIO 5
// Divider ratio calibrated against a multimeter reading on the pack (measured
// 4.10V real vs 3.72V computed with the theoretical 2.f ratio -> 2.f * 4.10/3.72).
#define RG_BATTERY_VDIV_RATIO        2.204f
#define RG_BATTERY_CALC_PERCENT(raw) (((raw) * RG_BATTERY_VDIV_RATIO - 3150.f) / (4100.f - 3150.f) * 100.f)
#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * RG_BATTERY_VDIV_RATIO * 0.001f)

// RG_GPIO_BOOST_EN (TPS22917 USB-C DAC/charge power-path) is NOT defined on
// this board revision: the load-switch mux is not fitted and GPIO38 is used
// for the 2nd LED (S-LED1) instead. The UAC host driver's VBUS management
// compiles out when this macro is absent.

// I2C bus. The former UART0 console pins are repurposed for I2C now that the
// serial console runs over USB Serial/JTAG (native USB D-/D+). External 4.7k
// pull-ups to 3.3V are present on the PCB.
//   GPIO43 (old U0TXD) = SDA
//   GPIO44 (old U0RXD) = SCL
// NOTE: requires CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y so UART0 stops driving
// these pins (set in the target sdkconfig).
#define RG_GPIO_I2C_SDA             GPIO_NUM_43
#define RG_GPIO_I2C_SCL             GPIO_NUM_44

// Real-time clock: NXP PCF85063A (PCF85063ATL) on the I2C bus, address 0x51.
// Battery-backed; read/written in rg_system_load_time()/rg_system_save_time().
#define RG_RTC_PCF85063             1
#define RG_RTC_I2C_ADDR             0x51
// Crystal load-capacitance select (Control_1 bit0): 0 = 7 pF, 1 = 12.5 pF.
#define RG_RTC_PCF85063_CAP_SEL     0

// SPI Display
#define RG_GPIO_LCD_MISO            GPIO_NUM_NC
#define RG_GPIO_LCD_MOSI            GPIO_NUM_12
#define RG_GPIO_LCD_CLK             GPIO_NUM_48
#define RG_GPIO_LCD_CS              GPIO_NUM_14
#define RG_GPIO_LCD_DC              GPIO_NUM_47
#define RG_GPIO_LCD_BCKL            GPIO_NUM_39
#define RG_GPIO_LCD_RST             GPIO_NUM_3

// SPI SD Card
#define RG_GPIO_SDSPI_MISO          GPIO_NUM_9
#define RG_GPIO_SDSPI_MOSI          GPIO_NUM_11
#define RG_GPIO_SDSPI_CLK           GPIO_NUM_13
#define RG_GPIO_SDSPI_CS            GPIO_NUM_10

// External I2S DAC
#define RG_GPIO_SND_I2S_BCK         41
#define RG_GPIO_SND_I2S_WS          42
#define RG_GPIO_SND_I2S_DATA        40
