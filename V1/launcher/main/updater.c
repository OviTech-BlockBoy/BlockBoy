// Firmware update over the internet.
//
// The launcher does the network work: fetch the manifest, compare versions,
// download the files to the SD card and verify them. It then writes a job and
// reboots into the 'flasher' app, because it cannot overwrite its own
// partition while running.
//
// Anything that goes wrong here is harmless: not a single byte of flash has
// been touched yet. Things only get serious once the flasher takes over, and
// that one re-checks every checksum first.

#include <rg_system.h>
#include <rg_ota.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>
#include <cJSON.h>

#include "gui.h"
#include "updater.h"

#if defined(RG_ENABLE_NETWORKING) && RG_UPDATER_ENABLE

#ifndef RG_OTA_MANIFEST_URL
#error "RG_OTA_MANIFEST_URL must be set in the target config.h to build the updater"
#endif

#define DOWNLOAD_BUFFER_SIZE (16 * 1024)

typedef struct
{
    char version[32];
    char notes[256];
    char date[32];
    int layout;
    rg_ota_part_t parts[RG_OTA_MAX_PARTS];
    char urls[RG_OTA_MAX_PARTS][256];
    size_t parts_count;
    size_t download_bytes;
} manifest_t;

// -----------------------------------------------------------------------------

// Compares "3.10.2"-style versions numerically per component. A string
// comparison would call "3.9.0" newer than "3.10.0".
static int version_compare(const char *a, const char *b)
{
    while (*a || *b)
    {
        int na = 0, nb = 0;

        while (*a >= '0' && *a <= '9')
            na = na * 10 + (*a++ - '0');
        while (*b >= '0' && *b <= '9')
            nb = nb * 10 + (*b++ - '0');

        if (na != nb)
            return na < nb ? -1 : 1;

        // Skip the separator; anything that is not a digit counts as a boundary.
        while (*a && (*a < '0' || *a > '9'))
            a++;
        while (*b && (*b < '0' || *b > '9'))
            b++;
    }
    return 0;
}

// -----------------------------------------------------------------------------

// Returns the manifest URL and reports whether it came from the test override.
//
// The override lives on the SD card, not in the firmware, so a single image
// works both on the workbench and at the customer's. If it is accidentally
// left in place, the dialog title shows it -- a device silently pulling
// updates from another address is exactly what you don't want.
static bool get_manifest_url(char *out, size_t len)
{
    char *override = rg_settings_get_string(RG_OTA_SETTINGS_NS, RG_OTA_SETTING_MANIFEST_URL, NULL);

    if (override && override[0])
    {
        RG_LOGW("Manifest URL overridden via config: '%s'", override);
        snprintf(out, len, "%s", override);
        free(override);
        return true;
    }

    free(override);
    snprintf(out, len, "%s", RG_OTA_MANIFEST_URL);
    return false;
}

static char *http_get_text(const char *url, size_t max_length)
{
    RG_LOGI("Fetching: '%s'", url);

    rg_http_req_t *req = rg_network_http_open(url, NULL);
    if (!req)
    {
        RG_LOGW("Connection to '%s' failed", url);
        return NULL;
    }

    if (req->status_code < 200 || req->status_code >= 300)
    {
        RG_LOGW("HTTP %d for '%s'", req->status_code, url);
        rg_network_http_close(req);
        return NULL;
    }

    char *buffer = calloc(1, max_length + 1);
    if (!buffer)
    {
        rg_network_http_close(req);
        return NULL;
    }

    size_t received = 0;
    int len;

    // http_read returns only part of the data per call; keep reading until 0 or full.
    while (received < max_length && (len = rg_network_http_read(req, buffer + received,
                                                               max_length - received)) > 0)
    {
        received += len;
    }

    rg_network_http_close(req);

    if (received == 0)
    {
        free(buffer);
        return NULL;
    }

    buffer[received] = 0;
    return buffer;
}

// -----------------------------------------------------------------------------

static bool parse_manifest(const char *json_text, manifest_t *out, char *error, size_t error_len)
{
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json_text);
    if (!root)
    {
        snprintf(error, error_len, _("Invalid update information"));
        return false;
    }

    bool ok = false;

    const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(root, "version"));
    const char *model = cJSON_GetStringValue(cJSON_GetObjectItem(root, "model"));
    const char *notes = cJSON_GetStringValue(cJSON_GetObjectItem(root, "notes"));
    const char *date = cJSON_GetStringValue(cJSON_GetObjectItem(root, "date"));
    cJSON *layout = cJSON_GetObjectItem(root, "layout");
    cJSON *files = cJSON_GetObjectItem(root, "files");

    if (!version || !files || !cJSON_IsArray(files))
    {
        snprintf(error, error_len, _("Invalid update information"));
        goto cleanup;
    }

    // A package for a different model would put app images at the wrong
    // offsets. The flasher refuses this as well, but here the message is
    // clearer and we save a multi-megabyte download.
    if (!model || strcmp(model, RG_OTA_MODEL) != 0)
    {
        snprintf(error, error_len, _("Update is for another model"));
        goto cleanup;
    }

    out->layout = layout ? (int)cJSON_GetNumberValue(layout) : RG_LAYOUT_VERSION;
    if (out->layout != RG_LAYOUT_VERSION)
    {
        // This device has an older flash layout. Updating over the air is no
        // longer possible; this has to go through the web flasher.
        snprintf(error, error_len, _("This update requires USB.\nSee blockboy.nl"));
        goto cleanup;
    }

    snprintf(out->version, sizeof(out->version), "%s", version);
    snprintf(out->notes, sizeof(out->notes), "%s", notes ?: "");
    snprintf(out->date, sizeof(out->date), "%s", date ?: "");

    int count = cJSON_GetArraySize(files);
    for (int i = 0; i < count; ++i)
    {
        if (out->parts_count >= RG_OTA_MAX_PARTS)
        {
            RG_LOGW("Manifest contains more than %d files, ignoring the rest", RG_OTA_MAX_PARTS);
            break;
        }

        cJSON *file = cJSON_GetArrayItem(files, i);
        const char *partition = cJSON_GetStringValue(cJSON_GetObjectItem(file, "partition"));
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(file, "url"));
        const char *sha256 = cJSON_GetStringValue(cJSON_GetObjectItem(file, "sha256"));
        cJSON *size = cJSON_GetObjectItem(file, "size");

        if (!partition || !url || !sha256 || !size || strlen(sha256) != 64)
        {
            snprintf(error, error_len, _("Invalid update information"));
            goto cleanup;
        }

        // The flasher runs from this partition and cannot replace itself.
        // Such a package should not exist, but better to refuse it here than
        // halfway through flashing.
        if (strcmp(partition, RG_OTA_FLASHER_PARTITION) == 0)
        {
            snprintf(error, error_len, _("Invalid update information"));
            goto cleanup;
        }

        size_t index = out->parts_count++;
        rg_ota_part_t *part = &out->parts[index];

        snprintf(part->partition, sizeof(part->partition), "%s", partition);
        snprintf(part->filename, sizeof(part->filename), "%s.bin", partition);
        snprintf(part->sha256, sizeof(part->sha256), "%s", sha256);
        part->size = (size_t)cJSON_GetNumberValue(size);
        snprintf(out->urls[index], sizeof(out->urls[index]), "%s", url);

        out->download_bytes += part->size;
    }

    if (out->parts_count == 0)
    {
        snprintf(error, error_len, _("Invalid update information"));
        goto cleanup;
    }

    ok = true;

cleanup:
    cJSON_Delete(root);
    return ok;
}

// -----------------------------------------------------------------------------

static bool download_file(const char *url, const char *path, const char *label,
                          size_t expected_size, int index, int count)
{
    RG_LOGI("Download '%s' -> '%s'", url, path);

    rg_http_req_t *req = rg_network_http_open(url, NULL);
    if (!req)
        return false;

    if (req->status_code < 200 || req->status_code >= 300)
    {
        RG_LOGW("HTTP %d for '%s'", req->status_code, url);
        rg_network_http_close(req);
        return false;
    }

    void *buffer = malloc(DOWNLOAD_BUFFER_SIZE);
    if (!buffer)
    {
        rg_network_http_close(req);
        return false;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp)
    {
        rg_network_http_close(req);
        free(buffer);
        return false;
    }

    size_t received = 0;
    int last_percent = -1;
    int len;
    bool ok = true;

    while ((len = rg_network_http_read(req, buffer, DOWNLOAD_BUFFER_SIZE)) > 0)
    {
        if (fwrite(buffer, 1, len, fp) != (size_t)len)
        {
            // Almost always a full or disconnected SD card.
            RG_LOGE("Writing to '%s' failed after %d bytes", path, (int)received);
            ok = false;
            break;
        }

        received += len;

        // The launcher's main loop stalls while we download. Without this
        // tick, system_monitor_task decides after 3 seconds that the app is
        // hung and starts painting "App unresponsive" over the screen -- which
        // causes a panic during the restart into the flasher.
        rg_system_tick(0);

        int percent = expected_size ? (int)((received * 100) / expected_size) : 0;
        if (percent != last_percent)
        {
            last_percent = percent;
            if (count > 1)
                rg_gui_draw_message("%s (%d/%d)\n%d%%", label, index + 1, count, percent);
            else
                rg_gui_draw_message("%s\n%d%%", label, percent);
        }
    }

    rg_network_http_close(req);
    free(buffer);

    if (fflush(fp) != 0 || ferror(fp))
        ok = false;
    fclose(fp);

    if (ok && expected_size && received != expected_size)
    {
        RG_LOGE("'%s': received %d of %d bytes", path, (int)received, (int)expected_size);
        ok = false;
    }

    if (!ok)
        rg_storage_delete(path);

    return ok;
}

static void hash_progress(size_t done, size_t total, void *arg)
{
    int *last = arg;
    // Hashing 7MB takes well over the 3 seconds the system monitor allows
    // before it considers the app hung.
    rg_system_tick(0);
    int percent = total ? (int)((done * 100) / total) : 0;
    if (percent == *last)
        return;
    *last = percent;
    rg_gui_draw_message("%s\n%d%%", _("Verifying"), percent);
}

// -----------------------------------------------------------------------------

static bool download_update(const manifest_t *manifest)
{
    rg_storage_mkdir(RG_BASE_PATH);
    rg_storage_mkdir(RG_OTA_PATH);

    // An old, half-finished job would mislead the flasher.
    rg_storage_delete(RG_OTA_JOB_FILE);

    // On the heap, not the stack: rg_ota_job_t is ~1.4KB and the main task
    // only has 8KB, a good chunk of which rg_gui_dialog has already used by
    // the time we get here. See the note in updater_show_dialog.
    rg_ota_job_t *job = calloc(1, sizeof(*job));
    if (!job)
    {
        rg_gui_alert(_("Update failed"), _("Out of memory."));
        return false;
    }

    snprintf(job->version, sizeof(job->version), "%s", manifest->version);

    // Everything is downloaded and flashed, even if a partition happens to
    // have the right contents already.
    //
    // We used to skip unchanged apps based on bookkeeping the flasher wrote
    // to ota.json on the SD card. Two problems: that bookkeeping describes
    // the flash of one device but travels with the card, so after a card swap
    // it can lie and a partition is left unflashed. And it gained nothing:
    // ESP-IDF stamps a build time into every binary, so nothing is ever
    // unchanged.
    int to_download = (int)manifest->parts_count;
    int done = 0;

    for (size_t i = 0; i < manifest->parts_count; ++i)
    {
        const rg_ota_part_t *part = &manifest->parts[i];
        char path[RG_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", RG_OTA_PATH, part->filename);

        if (!download_file(manifest->urls[i], path, _("Downloading"), part->size, done, to_download))
        {
            rg_gui_alert(_("Update failed"), _("Download failed.\nCheck your connection\nand SD card space."));
            free(job);
            return false;
        }

        // Verify immediately: a broken download is still free to discover at
        // this point. The flasher checks again later, but by then the user
        // would already have rebooted.
        int last = -1;
        char digest[65];
        if (!rg_ota_sha256_file(path, digest, hash_progress, &last))
        {
            rg_gui_alert(_("Update failed"), _("Could not read\ndownloaded file."));
            rg_storage_delete(path);
            free(job);
            return false;
        }

        if (strcasecmp(digest, part->sha256) != 0)
        {
            RG_LOGE("sha256 mismatch for %s", part->filename);
            rg_gui_alert(_("Update failed"), _("Downloaded file is\ncorrupted."));
            rg_storage_delete(path);
            free(job);
            return false;
        }

        job->parts[job->parts_count++] = *part;
        done++;
    }

    bool written = rg_ota_job_write(RG_OTA_JOB_FILE, job);
    free(job);

    if (!written)
    {
        rg_gui_alert(_("Update failed"), _("Could not write to\nSD card."));
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------

void updater_show_dialog(void)
{
    char error[128] = {0};

    if (rg_network_get_info().state != RG_NETWORK_CONNECTED)
    {
        rg_gui_alert(_("Firmware Update"), _("Connect to Wi-Fi first."));
        return;
    }

    char manifest_url[256];
    bool is_test_source = get_manifest_url(manifest_url, sizeof(manifest_url));

    rg_gui_draw_message("%s", _("Checking for updates..."));

    char *json_text = http_get_text(manifest_url, 16 * 1024);
    if (!json_text)
    {
        gui_redraw();
        rg_gui_alert(_("Firmware Update"), _("Could not reach\nthe update server."));
        return;
    }

    // manifest_t is ~3.7KB (mostly the 8 URLs of 256 bytes). That does not
    // fit on the stack: the main task gets 8KB (CONFIG_ESP_MAIN_TASK_STACK_SIZE)
    // and this function runs as a callback deep under rg_gui_dialog, which has
    // already taken a bite out of it. Putting it on the stack crashes hard
    // before the manifest is even fetched.
    manifest_t *manifest = calloc(1, sizeof(*manifest));
    if (!manifest)
    {
        free(json_text);
        gui_redraw();
        rg_gui_alert(_("Firmware Update"), _("Out of memory."));
        return;
    }

    bool parsed = parse_manifest(json_text, manifest, error, sizeof(error));
    free(json_text);
    gui_redraw();

    if (!parsed)
    {
        rg_gui_alert(_("Firmware Update"), error);
        free(manifest);
        return;
    }

    if (version_compare(RG_FIRMWARE_VERSION, manifest->version) >= 0)
    {
        char uptodate[128];
        snprintf(uptodate, sizeof(uptodate), "%s\n\n%s %s", _("You are up to date."),
                 _("Installed:"), RG_FIRMWARE_VERSION);
        rg_gui_alert(_("Firmware Update"), uptodate);
        free(manifest);
        return;
    }

    char message[512];
    snprintf(message, sizeof(message),
             "%s %s\n%s %s\n\n%s\n\n%s %d MB",
             _("Installed:"), RG_FIRMWARE_VERSION,
             _("Available:"), manifest->version,
             manifest->notes,
             _("Download:"), (int)((manifest->download_bytes + 524288) / 1048576));

    if (!rg_gui_confirm(is_test_source ? "Update Available [TEST]" : _("Update Available"),
                        message, true))
    {
        free(manifest);
        return;
    }

    if (!rg_storage_ready())
    {
        rg_gui_alert(_("Update failed"), _("No SD card.\nThe update is stored\nthere first."));
        free(manifest);
        return;
    }

    bool ready = download_update(manifest);
    free(manifest);

    if (!ready)
    {
        gui_redraw();
        return;
    }

    // From here on everything is downloaded and verified. The flasher takes
    // over; it lives in the factory partition and finds the job again by
    // itself if the power drops halfway.
    rg_gui_draw_message("%s", _("Restarting to install..."));
    rg_task_delay(1200);

    rg_system_switch_app(RG_OTA_FLASHER_PARTITION, "flasher", "", 0);
}

#endif // RG_ENABLE_NETWORKING && RG_UPDATER_ENABLE
