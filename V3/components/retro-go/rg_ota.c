#include "rg_system.h"
#include "rg_ota.h"

#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>

#define SHA_CHUNK 8192

static void bin_to_hex(const uint8_t *bin, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i)
    {
        out[i * 2 + 0] = digits[bin[i] >> 4];
        out[i * 2 + 1] = digits[bin[i] & 0x0F];
    }
    out[len * 2] = 0;
}

bool rg_ota_sha256_file(const char *path, char *hex_out, rg_ota_progress_t progress_cb, void *arg)
{
    RG_ASSERT_ARG(path && hex_out);

    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        RG_LOGE("Cannot open '%s'", path);
        return false;
    }

    // Buffer deliberately on the heap: the flasher has a small stack.
    uint8_t *buffer = malloc(SHA_CHUNK);
    if (!buffer)
    {
        fclose(fp);
        return false;
    }

    size_t total = 0;
    fseek(fp, 0, SEEK_END);
    total = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256, not SHA-224

    size_t done = 0;
    int len;
    while ((len = fread(buffer, 1, SHA_CHUNK, fp)) > 0)
    {
        mbedtls_sha256_update(&ctx, buffer, len);
        done += len;
        if (progress_cb)
            progress_cb(done, total, arg);
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    bool complete = !ferror(fp) && done == total;

    free(buffer);
    fclose(fp);

    if (!complete)
    {
        RG_LOGE("Read error in '%s' (%d of %d bytes)", path, (int)done, (int)total);
        return false;
    }

    bin_to_hex(digest, sizeof(digest), hex_out);
    return true;
}

static void set_error(char *error, size_t error_len, const char *msg)
{
    if (error && error_len)
        snprintf(error, error_len, "%s", msg);
}

bool rg_ota_job_read(const char *path, rg_ota_job_t *out, char *error, size_t error_len)
{
    RG_ASSERT_ARG(path && out);

    memset(out, 0, sizeof(*out));

    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        set_error(error, error_len, "No update found");
        return false;
    }

    char line[256];
    bool magic_ok = false;
    bool failed = false;

    while (fgets(line, sizeof(line), fp))
    {
        // Strip line endings so a trailing %s does not trip over \r.
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#')
            continue;

        int job_version = 0;
        if (!magic_ok)
        {
            if (sscanf(line, RG_OTA_JOB_MAGIC " %d", &job_version) != 1)
            {
                set_error(error, error_len, "Invalid update file");
                failed = true;
                break;
            }
            if (job_version != RG_OTA_JOB_VERSION)
            {
                set_error(error, error_len, "Update format too new");
                failed = true;
                break;
            }
            magic_ok = true;
            continue;
        }

        if (sscanf(line, "version %31s", out->version) == 1)
            continue;
        if (sscanf(line, "model %47s", out->model) == 1)
            continue;
        if (sscanf(line, "layout %d", &out->layout) == 1)
            continue;

        rg_ota_part_t part = {0};
        unsigned long size = 0;
        if (sscanf(line, "part %23s %63s %lu %64s", part.partition, part.filename, &size, part.sha256) == 4)
        {
            if (out->parts_count >= RG_OTA_MAX_PARTS)
            {
                set_error(error, error_len, "Too many partitions in update");
                failed = true;
                break;
            }
            if (strlen(part.sha256) != 64)
            {
                set_error(error, error_len, "Invalid checksum in update");
                failed = true;
                break;
            }
            part.size = size;
            out->parts[out->parts_count++] = part;
            continue;
        }

        RG_LOGW("Ignoring unknown job line: '%s'", line);
    }

    fclose(fp);

    if (failed)
        return false;

    if (!magic_ok || out->parts_count == 0)
    {
        set_error(error, error_len, "Update file is empty");
        return false;
    }

    // A package for a different model or a different partition layout would land
    // at the wrong offsets. That is exactly the scenario that bricks a device,
    // so stop hard here. If the field is missing, the package cannot be
    // trusted either.
    if (strcmp(out->model, RG_OTA_MODEL) != 0)
    {
        set_error(error, error_len, "Update is for a different model");
        return false;
    }

    if (out->layout != RG_LAYOUT_VERSION)
    {
        set_error(error, error_len, "Update requires USB flashing");
        return false;
    }

    return true;
}

bool rg_ota_job_write(const char *path, const rg_ota_job_t *job)
{
    RG_ASSERT_ARG(path && job);

    FILE *fp = fopen(path, "w");
    if (!fp)
    {
        RG_LOGE("Cannot write job '%s'", path);
        return false;
    }

    fprintf(fp, RG_OTA_JOB_MAGIC " %d\n", RG_OTA_JOB_VERSION);
    fprintf(fp, "version %s\n", job->version);
    fprintf(fp, "model %s\n", RG_OTA_MODEL);
    fprintf(fp, "layout %d\n", RG_LAYOUT_VERSION);

    for (size_t i = 0; i < job->parts_count; ++i)
    {
        const rg_ota_part_t *part = &job->parts[i];
        fprintf(fp, "part %s %s %lu %s\n", part->partition, part->filename,
                (unsigned long)part->size, part->sha256);
    }

    bool ok = ferror(fp) == 0;
    // fflush before fclose so a write error is still visible here.
    if (fflush(fp) != 0)
        ok = false;
    fclose(fp);

    if (!ok)
    {
        RG_LOGE("Write error in job '%s'", path);
        rg_storage_delete(path);
        return false;
    }

    rg_storage_commit();
    return true;
}
