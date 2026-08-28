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
static bool sdspi_device_added = false; // 0 is a valid handle, so track separately
static bool spi_bus_initialized = false;

// Forward declarations
static bool init_sd_card_for_usb(void);
static void deinit_sd_card_for_usb(void);

// --- Audio sink handover --------------------------------------------------
// USB MSC needs the USB-OTG controller in *device* mode. The "USB-C DAC" audio
// sink (UAC host) holds the same controller in *host* mode, and the USB host
// stack stays installed for the firmware's lifetime (UAC guide Pitfall #2), so it
// can't be freed at runtime — installing TinyUSB then fails ("USB connection
// failed"). The fix is to reboot: switch audio to "Ext DAC", reboot so the host
// stack is never loaded, run USB mode, then on disconnect restore USB-C DAC and
// reboot back. All automatic; the state is carried across the reboots in the
// "UsbModeBoot" NVS flag. See usb_msc_run_with_ui() and app_main's boot handler.
static void usb_audio_switch_sink(const char *target_name)
{
    size_t n = 0;
    const rg_audio_sink_t *sinks = rg_audio_get_sinks(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(sinks[i].name, target_name) == 0) {
            rg_audio_set_sink(sinks[i].driver->name, sinks[i].device);
            return;
        }
    }
}

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
    sdspi_device_added = true;

    // Host configuration for card init
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = RG_STORAGE_SDSPI_HOST;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    // Allocate card structure
    sd_card = malloc(sizeof(sdmmc_card_t));
    if (!sd_card) {
        RG_LOGE("Failed to allocate memory for SD card");
        sdspi_host_remove_device(sdspi_handle);
        sdspi_device_added = false;
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
        sdspi_device_added = false;
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

    if (sdspi_device_added) {
        sdspi_host_remove_device(sdspi_handle);
        sdspi_device_added = false;
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

    // Step 3: Configure MSC storage with SD card.
    // Storage BEFORE the driver: the moment TinyUSB is installed the device
    // enumerates and the host starts sending SCSI commands, and esp_tinyusb's
    // MSC callbacks dereference the storage handle without a NULL check. The
    // handle must therefore exist before the USB task can run.
    RG_LOGI("Configuring MSC storage...");

    const tinyusb_msc_sdmmc_config_t msc_sdmmc_cfg = {
        .card = sd_card,
        .callback_mount_changed = storage_mount_changed_cb,
        .callback_premount_changed = NULL,
        .mount_config = {
            .max_files = 5,
        },
    };

    esp_err_t ret = tinyusb_msc_storage_init_sdmmc(&msc_sdmmc_cfg);
    if (ret != ESP_OK) {
        RG_LOGE("Failed to initialize MSC storage: 0x%x", ret);
        deinit_sd_card_for_usb();
        rg_storage_init();
        return false;
    }

    // Step 4: Install TinyUSB; from here the host can see the disk
    RG_LOGI("Configuring TinyUSB...");

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,  // Use default
        .string_descriptor = NULL,  // Use default
        .string_descriptor_count = 0,
        .external_phy = false,
        .configuration_descriptor = NULL,  // Use default
    };

    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        RG_LOGE("Failed to install TinyUSB driver: 0x%x", ret);
        tinyusb_msc_storage_deinit();
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

    // Step 1: Uninstall the TinyUSB driver FIRST -- this stops the USB task,
    // so no MSC callback can fire anymore. Freeing the storage handle while
    // the task still runs lets the host's periodic "Test Unit Ready" poll
    // dereference the freed handle: a guaranteed panic whenever the cable is
    // still plugged into a PC.
    tinyusb_driver_uninstall();

    // Step 2: Unmount + deinit MSC storage, now that nothing can touch it
    tinyusb_msc_storage_unmount();
    tinyusb_msc_storage_deinit();

    // Step 3: Deinit SD card from USB
    deinit_sd_card_for_usb();

    // Step 4: Reinitialize storage for normal use
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
    // If audio is on "USB-C DAC", the UAC host owns the USB-OTG controller and the
    // host stack can't be freed at runtime (UAC guide Pitfall #2). Switch audio to
    // "Ext DAC", arm a USB-mode boot and reboot — on the next boot the host stack
    // isn't loaded, so USB device mode can take the controller. app_main's boot
    // handler calls us again after that reboot (audio is Ext DAC by then).
    const rg_audio_sink_t *cur = rg_audio_get_sink();
    if (cur && strcmp(cur->name, "USB-C DAC") == 0) {
        RG_LOGI("Audio is USB-C DAC; switching to Ext DAC and rebooting into USB mode");
        usb_audio_switch_sink("Ext DAC");
        rg_settings_set_number(NS_GLOBAL, "UsbModeBoot", 1);
        rg_settings_commit();
        rg_system_restart(); // does not return
    }

    // After the USB-mode reboot the boot's loading screen is still on the panel, and
    // rg_gui_alert only draws a centered box over whatever is there — clear to black
    // first so the "USB Connected" screen is clean.
    rg_display_clear(C_BLACK);

    // Start USB connection
    if (usb_msc_start()) {
        // Show status until user disconnects
        rg_gui_alert(_("USB Connected"), _("OK to disconnect"));
        usb_msc_stop();
    } else {
        rg_gui_alert(_("Error"), _("USB connection failed"));
    }

    // Came here via a USB-mode reboot (UsbModeBoot armed)? Restore the USB-C DAC sink
    // at runtime — the OTG controller is free again now that TinyUSB is uninstalled,
    // so the UAC host re-installs without needing a second reboot.
    if (rg_settings_get_number(NS_GLOBAL, "UsbModeBoot", 0)) {
        rg_settings_set_number(NS_GLOBAL, "UsbModeBoot", 0);
        rg_settings_commit();
        usb_audio_switch_sink("USB-C DAC");
    }
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
