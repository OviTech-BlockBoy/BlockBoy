// Target definition - N4R2 (4MB Flash, 2MB PSRAM) - GB/GBC only
#define RG_TARGET_NAME             "0v1Tech-BlockBoy-N4R2"

// 0v1Tech BlockBoy branding
#define RG_PROJECT_NAME            "BlockBoy"
// RG_PROJECT_VER is set via command line by rg_tool.py
#define RG_PROJECT_WEBSITE         "https://0v1tech.com"
#define RG_PROJECT_CREDITS         "BlockBoy by 0v1Tech\nBased on Retro-Go by ducalex\n"

// Storage
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDSPI_HOST       SPI3_HOST
#define RG_STORAGE_SDSPI_SPEED      SDMMC_FREQ_DEFAULT  // 20MHz (most stable)
// #define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_1
// #define RG_STORAGE_SDMMC_SPEED      SDMMC_FREQ_DEFAULT
// #define RG_STORAGE_FLASH_PARTITION  "vfs"

// Audio
#define RG_AUDIO_USE_INT_DAC        0   // 0 = Disable, 1 = GPIO25, 2 = GPIO26, 3 = Both
#define RG_AUDIO_USE_EXT_DAC        1   // 0 = Disable, 1 = Enable

// Video
#define RG_SCREEN_DRIVER            0   // 0 = ILI9341
#define RG_SCREEN_HOST              SPI2_HOST
#define RG_SCREEN_SPEED             SPI_MASTER_FREQ_40M // SPI_MASTER_FREQ_80M
#define RG_SCREEN_BACKLIGHT         1
#define RG_SCREEN_WIDTH             320
#define RG_SCREEN_HEIGHT            240
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_VISIBLE_AREA      {2, 0, 32, 7} // Left, Top, Right, Bottom 
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0} // Left, Top, Right, Bottom

// ILI9341 MADCTL (Memory Access Control) register 0x36
#define ILI9341_MADCTL 0x36
#define ILI9341_MADCTL_MY  0x80  // Row Address Order
#define ILI9341_MADCTL_MX  0x40  // Column Address Order
#define ILI9341_MADCTL_MV  0x20  // Row/Column Exchange
#define ILI9341_MADCTL_ML  0x10  // Vertical Refresh Order
#define ILI9341_MADCTL_BGR 0x08  // BGR color order
#define ILI9341_MADCTL_MH  0x04  // Horizontal Refresh Order


#define RG_SCREEN_INIT()                                                                                         \
    ILI9341_CMD(0xCF, 0x00, 0xc3, 0x30);                                                                         \
    ILI9341_CMD(0xED, 0x64, 0x03, 0x12, 0x81);                                                                   \
    ILI9341_CMD(0xE8, 0x85, 0x00, 0x78);                                                                         \
    ILI9341_CMD(0xCB, 0x39, 0x2c, 0x00, 0x34, 0x02);                                                             \
    ILI9341_CMD(0xF7, 0x20);                                                                                     \
    ILI9341_CMD(0xEA, 0x00, 0x00);                                                                               \
    ILI9341_CMD(0xC0, 0x1B);                 /* Power control   //VRH[5:0] */                                    \
    ILI9341_CMD(0xC1, 0x12);                 /* Power control   //SAP[2:0];BT[3:0] */                            \
    ILI9341_CMD(0xC5, 0x32, 0x3C);           /* VCM control */                                                   \
    ILI9341_CMD(0xC7, 0x91);                 /* VCM control2 */                                                  \
    ILI9341_CMD(ILI9341_MADCTL, 0xA8);  /* Memory Access Control (MY|MV|BGR) - Alternative ILI9341 variant */          \
    ILI9341_CMD(0xB1, 0x00, 0x10);           /* Frame Rate Control (1B=70, 1F=61, 10=119) */                     \
    ILI9341_CMD(0xB6, 0x0A, 0xA2);           /* Display Function Control */                                      \
    ILI9341_CMD(0xF6, 0x01, 0x30);                                                                               \
    ILI9341_CMD(0xF2, 0x00); /* 3Gamma Function Disable */                                                       \
    ILI9341_CMD(0xE0, 0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43, 0x47, 0x09, 0x15, 0x12, 0x16, 0x19);       \
    ILI9341_CMD(0xE1, 0xD0, 0x00, 0x05, 0x0D, 0x0C, 0x06, 0x2D, 0x44, 0x40, 0x0E, 0x1C, 0x18, 0x16, 0x19);


// Input
// Refer to rg_input.h to see all available RG_KEY_* and RG_GAMEPAD_*_MAP types
// PCB v3 - Corrected gamepad + volume buttons + brightness buttons (GPIO 1/2)
#define RG_GAMEPAD_GPIO_MAP {\
    {RG_KEY_UP,     .num = GPIO_NUM_16, .pullup = 1, .level = 0},\
    {RG_KEY_RIGHT,  .num = GPIO_NUM_15, .pullup = 1, .level = 0},\
    {RG_KEY_DOWN,   .num = GPIO_NUM_17, .pullup = 1, .level = 0},\
    {RG_KEY_LEFT,   .num = GPIO_NUM_6,  .pullup = 1, .level = 0},\
    {RG_KEY_SELECT, .num = GPIO_NUM_8,  .pullup = 1, .level = 0},\
    {RG_KEY_START,  .num = GPIO_NUM_46, .pullup = 1, .level = 0},\
    {RG_KEY_A,      .num = GPIO_NUM_7,  .pullup = 1, .level = 0},\
    {RG_KEY_B,      .num = GPIO_NUM_45, .pullup = 1, .level = 0},\
    {RG_KEY_L,      .num = GPIO_NUM_18, .pullup = 1, .level = 0},  /* Volume UP */\
    {RG_KEY_R,      .num = GPIO_NUM_21, .pullup = 1, .level = 0},  /* Volume DOWN */\
    {RG_KEY_X,      .num = GPIO_NUM_1,  .pullup = 1, .level = 0},  /* Brightness UP */\
    {RG_KEY_Y,      .num = GPIO_NUM_2,  .pullup = 1, .level = 0},  /* Brightness DOWN */\
}
// Virtual button combinations (no physical buttons needed)
#define RG_GAMEPAD_VIRT_MAP {\
    {RG_KEY_MENU,   .src = RG_KEY_START | RG_KEY_SELECT},\
    {RG_KEY_OPTION, .src = RG_KEY_SELECT | RG_KEY_A},\
}

// Battery
#define RG_BATTERY_DRIVER           1   // Enabled - battery monitoring active
#define RG_BATTERY_ADC_UNIT         ADC_UNIT_1
#define RG_BATTERY_ADC_CHANNEL      ADC_CHANNEL_4  // GPIO 5 (was ADC_CHANNEL_3 / GPIO 4)
#define RG_BATTERY_CALC_PERCENT(raw) (((raw) * 2.f - 3150.f) / (4100.f - 3150.f) * 100.f)
#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 2.f * 0.001f)

// Status LED (disabled - no LED mounted)
// #define RG_GPIO_LED                 GPIO_NUM_38

// SPI Display
#define RG_GPIO_LCD_MISO            GPIO_NUM_NC
#define RG_GPIO_LCD_MOSI            GPIO_NUM_12
#define RG_GPIO_LCD_CLK             GPIO_NUM_48  // Works on ESP32-S3 without V suffix (3.3V modules)
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
// #define RG_GPIO_SND_AMP_ENABLE      18
