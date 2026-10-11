// BlockBoy flasher -- writes a downloaded update to flash.
//
// This app runs from the 'factory' partition. That is deliberate:
//
//  * The launcher cannot overwrite its own partition while running.
//  * The bootloader picks factory whenever otadata is empty or invalid, so a
//    device that loses power halfway through an update ends up here again
//    instead of in a half-written launcher.
//
// The job stays on the SD card until everything is verified. If the update is
// interrupted, this app simply finds it again on the next boot and starts
// over. Only when everything checks out is the boot set back to the launcher.

#include <rg_system.h>
#include <rg_ota.h>

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <strings.h>

// Lives in spi_flash_mmap.h, but that header moves between IDF versions.
#ifndef SPI_FLASH_SEC_SIZE
#define SPI_FLASH_SEC_SIZE 4096
#endif

#define CHUNK_SIZE 4096

static rg_app_t *app;

// -----------------------------------------------------------------------------

static void draw_progress(const char *label, int index, int count, size_t done, size_t total)
{
    int percent = total ? (int)((done * 100) / total) : 0;
    if (count > 1)
        rg_gui_draw_message("%s (%d/%d)\n%d%%", label, index + 1, count, percent);
    else
        rg_gui_draw_message("%s\n%d%%", label, percent);
}

typedef struct
{
    const char *label;
    int index;
    int count;
    int last_percent;
} progress_ctx_t;

static void progress_cb(size_t done, size_t total, void *arg)
{
    progress_ctx_t *ctx = arg;
    // See the note in the launcher: without a tick the system monitor
    // considers the app hung and starts drawing.
    rg_system_tick(0);
    int percent = total ? (int)((done * 100) / total) : 0;
    // rg_gui_draw_message draws a whole little dialog; doing that per 4KB
    // block is slower than the flashing itself. Only redraw on a visible step.
    if (percent == ctx->last_percent)
        return;
    ctx->last_percent = percent;
    draw_progress(ctx->label, ctx->index, ctx->count, done, total);
}

// -----------------------------------------------------------------------------

static void boot_launcher(void)
{
    RG_LOGI("Back to the launcher");
    rg_system_switch_app(RG_OTA_LAUNCHER_PARTITION, "launcher", "", 0);
}

static void cleanup_job(const rg_ota_job_t *job)
{
    // Delete the job first: as long as it exists the next boot considers the
    // update unfinished. Only then clean up the (large) payloads.
    rg_storage_delete(RG_OTA_JOB_FILE);

    for (size_t i = 0; i < job->parts_count; ++i)
    {
        char path[RG_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", RG_OTA_PATH, job->parts[i].filename);
        rg_storage_delete(path);
    }

    rg_storage_commit();
}

// -----------------------------------------------------------------------------

// Checks ALL files before a single byte of flash is erased. Installing half
// an update because file 4 turned out corrupt is exactly what we don't want.
static bool verify_payloads(const rg_ota_job_t *job, char *error, size_t error_len)
{
    for (size_t i = 0; i < job->parts_count; ++i)
    {
        const rg_ota_part_t *part = &job->parts[i];
        char path[RG_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", RG_OTA_PATH, part->filename);

        const esp_partition_t *target = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, part->partition);

        if (!target)
        {
            snprintf(error, error_len, "%s\n%s", _("Unknown partition:"), part->partition);
            return false;
        }

        if (part->size > target->size)
        {
            snprintf(error, error_len, "%s\n'%s' (%u > %u)", _("Does not fit:"), part->partition,
                     (unsigned)part->size, (unsigned)target->size);
            return false;
        }

        // Overwriting ourselves while running is guaranteed to end badly.
        // The flasher should never be part of an OTA package anyway.
        if (target == esp_ota_get_running_partition())
        {
            snprintf(error, error_len, "%s", _("Update would overwrite\nthe installer itself"));
            return false;
        }

        progress_ctx_t ctx = {_("Checking update"), (int)i, (int)job->parts_count, -1};
        char digest[65];

        if (!rg_ota_sha256_file(path, digest, progress_cb, &ctx))
        {
            snprintf(error, error_len, "%s\n%s", _("Cannot read file:"), part->filename);
            return false;
        }

        if (strcasecmp(digest, part->sha256) != 0)
        {
            RG_LOGE("sha256 mismatch for %s: %s != %s", part->filename, digest, part->sha256);
            snprintf(error, error_len, "%s\n%s", _("Corrupted file:"), part->filename);
            return false;
        }
    }

    return true;
}

// Refuses a package older than what is already on the device.
//
// ESP-IDF's own anti-rollback (CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK) is not
// available here: it requires a partition table without a factory partition,
// and this app IS the factory partition. It also spends one of only 16 eFuse
// bits per bump. So downgrade protection lives in software instead, which stops
// an attacker replaying an old but genuinely signed package to reopen a hole
// that has since been closed.
//
// The reference is the launcher actually installed, read from its app
// descriptor, not this app's own version: the flasher is not updated over the
// air, so its compiled-in version drifts behind and would reject good packages.
static bool check_not_a_downgrade(const rg_ota_job_t *job, char *error, size_t error_len)
{
    const esp_partition_t *launcher = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "launcher");

    esp_app_desc_t installed;
    if (!launcher || esp_ota_get_partition_description(launcher, &installed) != ESP_OK)
    {
        // No readable launcher: the device is half-flashed or brand new, which
        // is exactly when an update has to be allowed through.
        RG_LOGW("No installed version to compare against, allowing the update");
        return true;
    }

    if (rg_ota_version_compare(job->version, installed.version) < 0)
    {
        RG_LOGE("Refusing downgrade: package %s is older than installed %s",
                job->version, installed.version);
        snprintf(error, error_len, "%s\n%s %s\n%s %s", _("This update is older\nthan what is installed."),
                 _("Installed:"), installed.version, _("Package:"), job->version);
        return false;
    }

    return true;
}

static bool flash_part(const rg_ota_part_t *part, int index, int count, char *error, size_t error_len)
{
    const esp_partition_t *target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, part->partition);
    RG_ASSERT(target, "partition disappeared between check and write");

    char path[RG_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", RG_OTA_PATH, part->filename);

    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        snprintf(error, error_len, "%s\n%s", _("Cannot open file:"), part->filename);
        return false;
    }

    uint8_t *buffer = malloc(CHUNK_SIZE);
    if (!buffer)
    {
        fclose(fp);
        snprintf(error, error_len, "%s", _("Out of memory"));
        return false;
    }

    // Written through esp_ota_ops rather than straight into the partition,
    // because esp_ota_end() validates the image before we ever let the
    // bootloader near it -- and with signed apps enabled that validation
    // includes the RSA signature. That check is the whole point of this
    // function: it is what separates an update built by OviTech from one that
    // was not, and it guards BOTH routes in, over the air and straight off the
    // SD card, because both of them end up here.
    //
    // esp_ota_begin erases only the space the image needs, same as the
    // hand-rolled erase it replaces.
    //
    // Note the order: the signature is checked by esp_ota_end, so it is checked
    // AFTER the partition has been erased and written. A package signed by
    // somebody else therefore leaves that partition invalid. That is survivable
    // by design -- this app is the factory partition, the bootloader falls back
    // to it, and the user lands right back in this screen to retry with a good
    // package. Checking the signature straight from the file first would mean
    // recomputing the image digest exactly the way the ROM does, and getting
    // that subtly wrong would reject good updates, which is the worse failure.
    esp_ota_handle_t handle = 0;
    bool ota_open = false;

    draw_progress(_("Erasing"), index, count, 0, 1);
    // Erasing ~1.7MB is a single blocking call of several seconds; report
    // that we are alive again right after.
    rg_system_tick(0);
    esp_err_t err = esp_ota_begin(target, part->size, &handle);
    rg_system_tick(0);
    if (err != ESP_OK)
    {
        RG_LOGE("esp_ota_begin(%s, %u) = 0x%X", part->partition, (unsigned)part->size, err);
        snprintf(error, error_len, "%s\n(%s)", _("Erase failed"), part->partition);
        goto fail;
    }
    ota_open = true;

    size_t written = 0;
    int last_percent = -1;

    while (written < part->size)
    {
        size_t want = RG_MIN((size_t)CHUNK_SIZE, part->size - written);
        size_t got = fread(buffer, 1, want, fp);
        if (got != want)
        {
            snprintf(error, error_len, "%s\n%s", _("Read error:"), part->filename);
            goto fail;
        }

        err = esp_ota_write(handle, buffer, got);
        if (err != ESP_OK)
        {
            RG_LOGE("esp_ota_write(%s, %u) = 0x%X", part->partition, (unsigned)written, err);
            snprintf(error, error_len, "%s\n(%s)", _("Write failed"), part->partition);
            goto fail;
        }

        written += got;
        rg_system_tick(0);

        int percent = (int)((written * 100) / part->size);
        if (percent != last_percent)
        {
            last_percent = percent;
            draw_progress(_("Installing"), index, count, written, part->size);
        }
    }

    // Closes the handle either way, so it must not be aborted after this.
    ota_open = false;
    err = esp_ota_end(handle);
    if (err != ESP_OK)
    {
        RG_LOGE("esp_ota_end(%s) = 0x%X", part->partition, err);
        if (err == ESP_ERR_OTA_VALIDATE_FAILED)
        {
            // Either the signature does not match our key, or it is missing.
            // Worth its own message: this is not a broken download but firmware
            // from somebody else, and the user should be told that plainly.
            snprintf(error, error_len, "%s\n(%s)", _("Update is not signed\nby OviTech"), part->partition);
        }
        else
        {
            snprintf(error, error_len, "%s\n(%s)", _("Write failed"), part->partition);
        }
        goto fail;
    }

    free(buffer);
    fclose(fp);
    return true;

fail:
    if (ota_open)
        esp_ota_abort(handle);
    free(buffer);
    fclose(fp);
    return false;
}

// Reads back the freshly written flash and compares the hash. Without this
// step a silent write error would only surface at boot time, when the flasher
// is no longer in play.
static bool verify_part(const rg_ota_part_t *part, int index, int count, char *error, size_t error_len)
{
    const esp_partition_t *target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, part->partition);
    RG_ASSERT(target, "partition disappeared between write and verify");

    uint8_t *buffer = malloc(CHUNK_SIZE);
    if (!buffer)
    {
        snprintf(error, error_len, "%s", _("Out of memory"));
        return false;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    size_t done = 0;
    int last_percent = -1;
    bool ok = true;

    while (done < part->size)
    {
        size_t want = RG_MIN((size_t)CHUNK_SIZE, part->size - done);
        if (esp_partition_read(target, done, buffer, want) != ESP_OK)
        {
            snprintf(error, error_len, "%s\n(%s)", _("Verify read failed"), part->partition);
            ok = false;
            break;
        }
        mbedtls_sha256_update(&ctx, buffer, want);
        done += want;
        rg_system_tick(0);

        int percent = (int)((done * 100) / part->size);
        if (percent != last_percent)
        {
            last_percent = percent;
            draw_progress(_("Verifying"), index, count, done, part->size);
        }
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    free(buffer);

    if (!ok)
        return false;

    char hex[65];
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i)
    {
        hex[i * 2 + 0] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0F];
    }
    hex[64] = 0;

    if (strcasecmp(hex, part->sha256) != 0)
    {
        RG_LOGE("Flash verification failed for %s: %s != %s", part->partition, hex, part->sha256);
        snprintf(error, error_len, "%s\n(%s)", _("Verification failed"), part->partition);
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------

void app_main(void)
{
    const rg_handlers_t handlers = {0};

    app = rg_system_init(32000, &handlers, NULL);
    app->configNs = "flasher";

    rg_display_clear(C_BLACK);

    if (!rg_storage_ready())
    {
        // Without an SD card there is nothing to install. Deliberately no
        // message: after a fresh web flash otadata is empty and the device
        // boots here once. Someone who hasn't inserted a card yet would get an
        // error at the very first start about something they never asked for.
        // Anyone who did start an update is simply offered it again once the
        // launcher runs.
        RG_LOGI("No storage available, continuing straight to the launcher");
        boot_launcher();
    }

    // On the heap: rg_ota_job_t is ~1.4KB and the main task has 8. The
    // flasher is the route by which a half-flashed device becomes whole again,
    // so a stack overflow here is the last thing you want.
    rg_ota_job_t *job = calloc(1, sizeof(*job));
    char error[128] = {0};

    if (!job)
    {
        RG_LOGE("No memory for the update job");
        boot_launcher();
    }

    if (!rg_ota_job_read(RG_OTA_JOB_FILE, job, error, sizeof(error)))
    {
        // No job = normal boot via factory (e.g. after a factory flash).
        // Continue silently; only a genuinely broken job deserves a message.
        if (rg_storage_stat(RG_OTA_JOB_FILE).is_file)
        {
            rg_gui_alert(_("Update failed"), error);
            rg_storage_delete(RG_OTA_JOB_FILE);
        }
        boot_launcher();
    }

    RG_LOGI("Found update %s, %d partition(s)", job->version, (int)job->parts_count);

    if (!check_not_a_downgrade(job, error, sizeof(error)))
    {
        // Nothing has been erased yet, so the device is still whole. Drop the
        // job so the same package does not ask again on every boot.
        rg_gui_alert(_("Update refused"), error);
        cleanup_job(job);
        boot_launcher();
    }

    while (true)
    {
        if (!verify_payloads(job, error, sizeof(error)))
        {
            // The payloads are bad and flashing has not started, so the
            // device is still fully intact. Clean up and boot normally.
            rg_gui_alert(_("Update failed"), error);
            cleanup_job(job);
            boot_launcher();
        }

        bool ok = true;

        for (size_t i = 0; i < job->parts_count && ok; ++i)
        {
            ok = flash_part(&job->parts[i], (int)i, (int)job->parts_count, error, sizeof(error))
              && verify_part(&job->parts[i], (int)i, (int)job->parts_count, error, sizeof(error));
        }

        if (ok)
            break;

        // From here on the flash may be half-written. The job stays put, so
        // powering off and on lands here again. Booting into a possibly broken
        // launcher is only offered as a last resort.
        //
        // The question must be in the text: a yes/no dialog with only an error
        // message above it leaves the user guessing what 'yes' means.
        char retry_message[256];
        snprintf(retry_message, sizeof(retry_message), "%s\n\n%s", error, _("Try again?"));

        if (!rg_gui_confirm(_("Update failed"), retry_message, true))
        {
            if (rg_gui_confirm(_("Start anyway?"),
                               _("The installation is not\n"
                                 "complete. The device may\n"
                                 "not work.\n\n"
                                 "Try to start anyway?"), false))
            {
                boot_launcher();
            }
        }
    }

    rg_gui_draw_message("%s %s", _("Installed version"), job->version);
    cleanup_job(job);
    free(job);
    rg_task_delay(1500);

    boot_launcher();
}
