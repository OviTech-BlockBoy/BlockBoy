#define RG_LOG_TAG "USB_MSC"

#include "usb_msc.h"
#include <rg_system.h>
#include <string.h>

#if RG_USB_MSC_SUPPORTED

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <sdmmc_cmd.h>
#include <esp_vfs_fat.h>
#include <tinyusb.h>
#include <tusb_msc_storage.h>

// USB MSC state
static bool usb_msc_active = false;
static bool usb_msc_mounted = false;
static sdmmc_card_t *sd_card = NULL;
static sdspi_dev_handle_t sdspi_handle = 0;
static bool spi_bus_initialized = false;

// Forward declarations
static bool init_sd_card_for_usb(void);
static void deinit_sd_card_for_usb(void);

// TinyUSB MSC callbacks
static void storage_mount_changed_cb(tinyusb_msc_event_t *event)
{
    if (event->mount_changed_data.is_mounted) {
        RG_LOGI("USB MSC mounted by host");
        usb_msc_mounted = true;
    } else {
        RG_LOGI("USB MSC unmounted by host");
        usb_msc_mounted = false;
    }
}

void usb_msc_init(void)
{
    RG_LOGI("USB MSC subsystem initialized");
}

static bool init_sd_card_for_usb(void)
{
    esp_err_t ret;

    // SD card SPI configuration from config.h
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = RG_GPIO_SDSPI_MOSI,
        .miso_io_num = RG_GPIO_SDSPI_MISO,
        .sclk_io_num = RG_GPIO_SDSPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    // Initialize SPI bus
    ret = spi_bus_initialize(RG_STORAGE_SDSPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret == ESP_OK) {
        spi_bus_initialized = true;
    } else if (ret == ESP_ERR_INVALID_STATE) {
        // Already initialized, that's ok
        spi_bus_initialized = false;
    } else {
        RG_LOGE("Failed to initialize SPI bus: 0x%x", ret);
        return false;
    }

    // Initialize SDSPI host
    ret = sdspi_host_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        RG_LOGE("Failed to initialize SDSPI host: 0x%x", ret);
        if (spi_bus_initialized) spi_bus_free(RG_STORAGE_SDSPI_HOST);
        return false;
    }

    // SD card device configuration
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = RG_STORAGE_SDSPI_HOST;
    slot_config.gpio_cs = RG_GPIO_SDSPI_CS;

    // Add the SD card device
    ret = sdspi_host_init_device(&slot_config, &sdspi_handle);
    if (ret != ESP_OK) {
        RG_LOGE("Failed to add SD SPI device: 0x%x", ret);
        sdspi_host_deinit();
        if (spi_bus_initialized) spi_bus_free(RG_STORAGE_SDSPI_HOST);
        return false;
    }

    // Host configuration for card init
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = RG_STORAGE_SDSPI_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    // Allocate card structure
    sd_card = malloc(sizeof(sdmmc_card_t));
    if (!sd_card) {
        RG_LOGE("Failed to allocate memory for SD card");
        sdspi_host_remove_device(sdspi_handle);
        sdspi_host_deinit();
        if (spi_bus_initialized) spi_bus_free(RG_STORAGE_SDSPI_HOST);
        return false;
    }

    // Initialize the card
    ret = sdmmc_card_init(&host, sd_card);
    if (ret != ESP_OK) {
        RG_LOGE("Failed to initialize SD card: 0x%x", ret);
        free(sd_card);
        sd_card = NULL;
        sdspi_host_remove_device(sdspi_handle);
        sdspi_host_deinit();
        if (spi_bus_initialized) spi_bus_free(RG_STORAGE_SDSPI_HOST);
        return false;
    }

    RG_LOGI("SD card initialized for USB MSC: %s, %llu MB",
            sd_card->cid.name,
            (uint64_t)sd_card->csd.capacity * sd_card->csd.sector_size / (1024 * 1024));

    return true;
}

static void deinit_sd_card_for_usb(void)
{
    if (sd_card) {
        free(sd_card);
        sd_card = NULL;
    }

    if (sdspi_handle != 0) {
        sdspi_host_remove_device(sdspi_handle);
        sdspi_handle = 0;
    }

    sdspi_host_deinit();

    if (spi_bus_initialized) {
        spi_bus_free(RG_STORAGE_SDSPI_HOST);
        spi_bus_initialized = false;
    }

    RG_LOGI("SD card deinitialized from USB MSC");
}

bool usb_msc_start(void)
{
    if (usb_msc_active) {
        RG_LOGW("USB MSC already active");
        return true;
    }

    RG_LOGI("Starting USB MSC mode...");

    // Step 1: Unmount the SD card from the filesystem
    RG_LOGI("Unmounting SD card from filesystem...");
    rg_storage_deinit();

    // Step 2: Initialize SD card for USB access
    if (!init_sd_card_for_usb()) {
        RG_LOGE("Failed to initialize SD card for USB");
        // Try to remount for normal use
        rg_storage_init();
        return false;
    }

    // Step 3: Configure TinyUSB
    RG_LOGI("Configuring TinyUSB...");

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,  // Use default
        .string_descriptor = NULL,  // Use default
        .string_descriptor_count = 0,
        .external_phy = false,
        .configuration_descriptor = NULL,  // Use default
    };

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        RG_LOGE("Failed to install TinyUSB driver: 0x%x", ret);
        deinit_sd_card_for_usb();
        rg_storage_init();
        return false;
    }

    // Step 4: Configure MSC storage with SD card
    RG_LOGI("Configuring MSC storage...");

    const tinyusb_msc_sdmmc_config_t msc_sdmmc_cfg = {
        .card = sd_card,
        .callback_mount_changed = storage_mount_changed_cb,
        .callback_premount_changed = NULL,
        .mount_config = {
            .max_files = 5,
        },
    };

    ret = tinyusb_msc_storage_init_sdmmc(&msc_sdmmc_cfg);
    if (ret != ESP_OK) {
        RG_LOGE("Failed to initialize MSC storage: 0x%x", ret);
        tinyusb_driver_uninstall();
        deinit_sd_card_for_usb();
        rg_storage_init();
        return false;
    }

    // Mount the storage for USB access
    ret = tinyusb_msc_storage_mount(RG_STORAGE_ROOT);
    if (ret != ESP_OK) {
        RG_LOGW("Failed to mount MSC storage (may be ok): 0x%x", ret);
        // This is not fatal, the USB host will mount it
    }

    usb_msc_active = true;
    RG_LOGI("USB MSC mode started successfully!");
    RG_LOGI("Connect USB cable to access SD card from computer");

    return true;
}

bool usb_msc_stop(void)
{
    if (!usb_msc_active) {
        RG_LOGW("USB MSC not active");
        return true;
    }

    RG_LOGI("Stopping USB MSC mode...");

    // Step 1: Unmount MSC storage
    tinyusb_msc_storage_unmount();

    // Step 2: Deinit MSC storage
    tinyusb_msc_storage_deinit();

    // Step 3: Uninstall TinyUSB driver
    tinyusb_driver_uninstall();

    // Step 4: Deinit SD card from USB
    deinit_sd_card_for_usb();

    // Step 5: Reinitialize storage for normal use
    RG_LOGI("Remounting SD card for normal use...");
    rg_storage_init();

    usb_msc_active = false;
    usb_msc_mounted = false;

    RG_LOGI("USB MSC mode stopped, normal operation resumed");
    return true;
}

bool usb_msc_is_active(void)
{
    return usb_msc_active;
}

void usb_msc_run_with_ui(void)
{
    // Start USB connection
    if (!usb_msc_start()) {
        rg_gui_alert(_("Error"), _("USB connection failed"));
        return;
    }

    // Show status until user disconnects
    rg_gui_alert(_("USB Connected"), _("OK to disconnect"));

    // Stop USB connection
    usb_msc_stop();
}

#else // !RG_USB_MSC_SUPPORTED

void usb_msc_init(void)
{
    RG_LOGW("USB MSC not supported on this platform");
}

bool usb_msc_start(void)
{
    RG_LOGE("USB MSC not supported");
    return false;
}

bool usb_msc_stop(void)
{
    return true;
}

bool usb_msc_is_active(void)
{
    return false;
}

void usb_msc_run_with_ui(void)
{
    rg_gui_alert(_("Not Supported"), _("USB MSC is not available\non this hardware."));
}

#endif // RG_USB_MSC_SUPPORTED
