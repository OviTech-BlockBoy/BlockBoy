#include "rg_system.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(RG_STORAGE_SDSPI_HOST)
#include <driver/sdspi_host.h>
#define SDCARD_DO_TRANSACTION sdspi_host_do_transaction
#elif defined(RG_STORAGE_SDMMC_HOST)
#include <driver/sdmmc_host.h>
#define SDCARD_DO_TRANSACTION sdmmc_host_do_transaction
#endif

#ifdef ESP_PLATFORM
#include <esp_vfs_fat.h>
#endif

// TEST 1 (§6.59 handoff): decisive multi-block vs single-block raw SD read benchmark.
// Proves whether CMD18 multi-block actually beats 64x CMD17 single-block on THIS card,
// before investing in a v5.5 bypass or v6.0 migration. Exposed via rg_storage.h and
// invoked from the in-game benchmark (rg_benchmark), which records the result in its
// CSV header — so it runs on demand while the game is paused, not at every boot.
#if defined(RG_STORAGE_SDSPI_HOST) || defined(RG_STORAGE_SDMMC_HOST)
#include <sdmmc_cmd.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#include <windows.h>
#define access _access
#define mkdir(A, B) mkdir(A)
#if defined(__MINGW32__)
#include <dirent.h>
#endif
#else
#include <dirent.h>
#include <unistd.h>
#endif

static bool disk_mounted = false;
#if defined(RG_STORAGE_SDSPI_HOST) || defined(RG_STORAGE_SDMMC_HOST)
static sdmmc_card_t *card_handle = NULL;
#endif
#if defined(RG_STORAGE_FLASH_PARTITION)
static wl_handle_t wl_handle = WL_INVALID_HANDLE;
#endif

#define CHECK_PATH(path)          \
    if (!(path && path[0]))       \
    {                             \
        RG_LOGE("No path given"); \
        return false;             \
    }

#if defined(RG_STORAGE_SDSPI_HOST) || defined(RG_STORAGE_SDMMC_HOST)
static esp_err_t sdcard_do_transaction(int slot, sdmmc_command_t *cmdinfo)
{
    rg_system_set_indicator(RG_INDICATOR_ACTIVITY_DISK, 1);

    esp_err_t ret = SDCARD_DO_TRANSACTION(slot, cmdinfo);
    if (ret == ESP_ERR_NO_MEM)
    {
        // free some memory and try again?
    }

    rg_system_set_indicator(RG_INDICATOR_ACTIVITY_DISK, 0);
    return ret;
}
#endif

#if defined(RG_STORAGE_SDSPI_HOST) || defined(RG_STORAGE_SDMMC_HOST)
// ---- §6.60 raw ROM reader: direct multi-block sdmmc reads, bypassing FatFS/fread ----------
// See rg_storage.h for rationale + concurrency contract. The win (TEST 1, 2.14x) comes from
// calling sdmmc_read_sectors() directly with an INTERNAL DMA buffer (-> CMD18 multi-block),
// instead of fread() into PSRAM (-> the v5.5 driver's 64x CMD17 single-block fallback).

struct rg_rawrom
{
    FIL       fil;             // open FatFS handle (kept for the fast-seek CLMT)
    uint32_t *cltbl;           // cluster link map table (fast-seek), malloc'd
    uint8_t  *scratch;         // internal DMA-capable bounce buffer
    uint32_t  scratch_sectors; // its size in 512-byte sectors
    uint16_t  csize;           // sectors per cluster
    uint32_t  database;        // first data sector (absolute card LBA of cluster 2)
};

// Map a cluster-index (0-based from start of file) to its absolute cluster number using the
// CLMT. Also reports how many clusters remain contiguous from that point (within the fragment),
// so callers can read across cluster boundaries in one transaction. Returns 0 on error.
static uint32_t rawrom_clmt_lookup(const uint32_t *cltbl, uint32_t ci, uint32_t *contig_clusters)
{
    const uint32_t *tbl = cltbl + 1;   // skip cltbl[0] (table item count)
    uint32_t cl = ci;
    for (;;)
    {
        uint32_t ncl = *tbl++;         // number of clusters in this fragment
        if (ncl == 0) { if (contig_clusters) *contig_clusters = 0; return 0; }  // end of table
        uint32_t top = *tbl++;         // first cluster of the fragment (contiguous run)
        if (cl < ncl) { if (contig_clusters) *contig_clusters = ncl - cl; return top + cl; }
        cl -= ncl;
    }
}

rg_rawrom_t *rg_storage_rawrom_open(const char *vfs_path)
{
    if (card_handle == NULL || vfs_path == NULL)
        return NULL;

    // Translate the VFS path ("/sd/roms/..") to a FatFS path ("0:/roms/..").
    // Same "drive 0" assumption as rg_storage_get_free_space().
    size_t rootlen = strlen(RG_STORAGE_ROOT);
    if (strncmp(vfs_path, RG_STORAGE_ROOT, rootlen) != 0)
    {
        RG_LOGW("[RAWROM] path '%s' not under " RG_STORAGE_ROOT, vfs_path);
        return NULL;
    }
    char ffpath[300];
    snprintf(ffpath, sizeof(ffpath), "0:%s", vfs_path + rootlen);

    rg_rawrom_t *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;

    if (f_open(&h->fil, ffpath, FA_READ) != FR_OK)
    {
        RG_LOGW("[RAWROM] f_open('%s') failed", ffpath);
        free(h);
        return NULL;
    }

    FATFS *fs = h->fil.obj.fs;
    h->csize    = fs->csize;
    h->database = (uint32_t)fs->database;

    // Build the cluster link map table (fast-seek). Probe required size first (a 1-entry table
    // is intentionally too small -> CREATE_LINKMAP writes the required item count into cltbl[0]).
    uint32_t probe = 1;
    h->fil.cltbl = &probe;
    f_lseek(&h->fil, CREATE_LINKMAP);          // expected FR_NOT_ENOUGH_CORE; sets probe = needed
    uint32_t need = probe;
    h->fil.cltbl = NULL;
    if (need < 4)                              // 2 header + at least one [ncl,tcl] pair
        goto fail;
    h->cltbl = malloc(need * sizeof(uint32_t));
    if (!h->cltbl)
        goto fail;
    h->cltbl[0] = need;
    h->fil.cltbl = h->cltbl;
    if (f_lseek(&h->fil, CREATE_LINKMAP) != FR_OK)
        goto fail;

    // Internal DMA scratch: bigger = fewer transactions/page. Try 32KB, fall back to 8KB.
    for (uint32_t kb = 32; kb >= 8; kb >>= 1)
    {
        h->scratch = heap_caps_malloc(kb * 1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (h->scratch) { h->scratch_sectors = kb * 1024 / 512; break; }
    }
    if (!h->scratch)
        goto fail;

    RG_LOGI("[RAWROM] ready: csize=%u sect/clust, %u extents, scratch=%uKB, database=%u",
            (unsigned)h->csize, (unsigned)((need - 2) / 2),
            (unsigned)(h->scratch_sectors / 2), (unsigned)h->database);
    return h;

fail:
    if (h->cltbl) free(h->cltbl);
    f_close(&h->fil);
    free(h);
    return NULL;
}

bool rg_storage_rawrom_read(rg_rawrom_t *h, uint32_t file_offset, void *dst, uint32_t len)
{
    if (!h || card_handle == NULL || (file_offset | len) & 0x1FF)   // must be 512-aligned
        return false;

    uint8_t *out = (uint8_t *)dst;
    uint32_t remaining = len / 512;            // sectors still to read
    uint32_t foff      = file_offset;
    const uint32_t clust_bytes = (uint32_t)h->csize * 512u;

    while (remaining)
    {
        uint32_t ci            = foff / clust_bytes;          // cluster index from start of file
        uint32_t sect_in_clust = (foff / 512u) % h->csize;    // sector offset within that cluster
        uint32_t contig;
        uint32_t clust = rawrom_clmt_lookup(h->cltbl, ci, &contig);
        if (clust < 2)
            return false;

        LBA_t    lba = (LBA_t)h->database + (LBA_t)h->csize * (clust - 2) + sect_in_clust;
        // Sectors physically contiguous from here = rest of the contiguous cluster fragment.
        uint32_t run = contig * h->csize - sect_in_clust;

        while (run && remaining)
        {
            uint32_t n = run;
            if (n > remaining)            n = remaining;
            if (n > h->scratch_sectors)   n = h->scratch_sectors;
            if (sdmmc_read_sectors(card_handle, h->scratch, lba, n) != ESP_OK)
                return false;
            memcpy(out, h->scratch, (size_t)n * 512);
            out += n * 512; lba += n; foff += n * 512;
            run -= n; remaining -= n;
        }
    }
    return true;
}

void rg_storage_rawrom_close(rg_rawrom_t *h)
{
    if (!h)
        return;
    if (h->scratch) heap_caps_free(h->scratch);
    if (h->cltbl)   free(h->cltbl);
    f_close(&h->fil);
    free(h);
}
#else  // not an SD-host build: stubs so callers link (they fall back to fread)
rg_rawrom_t *rg_storage_rawrom_open(const char *vfs_path) { (void)vfs_path; return NULL; }
bool rg_storage_rawrom_read(rg_rawrom_t *h, uint32_t o, void *d, uint32_t l) { (void)h; (void)o; (void)d; (void)l; return false; }
void rg_storage_rawrom_close(rg_rawrom_t *h) { (void)h; }
#endif

// The firmware's folder on the SD card used to be "retro-go" and is now
// "BlockBoy". Cards that were set up before the rename still carry the old
// name, so rename it once, right after mounting: without this the device would
// silently start from an empty folder and the owner's saves, settings and BIOS
// files would appear to be gone (they'd still be on the card, just unread).
// Only the firmware's own folder moves -- /roms, /romart and /music sit in the
// card root and are left untouched.
static void migrate_base_path(void)
{
    struct stat st;

    if (stat(RG_BASE_PATH, &st) == 0)
        return; // already migrated (or a fresh card that just got the folder)

    if (stat(RG_BASE_PATH_LEGACY, &st) != 0)
        return; // nothing to migrate

    if (rename(RG_BASE_PATH_LEGACY, RG_BASE_PATH) == 0)
        RG_LOGI("Renamed '%s' to '%s'", RG_BASE_PATH_LEGACY, RG_BASE_PATH);
    else
        RG_LOGE("Could not rename '%s' to '%s' (errno %d). Saves and settings "
                "stay in the old folder!", RG_BASE_PATH_LEGACY, RG_BASE_PATH, errno);
}

void rg_storage_init(void)
{
    RG_ASSERT(!disk_mounted, "Storage already initialized!");
    int error_code = -1;

#if defined(RG_STORAGE_SDSPI_HOST)

    RG_LOGI("Looking for SD Card using SDSPI...");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = RG_GPIO_SDSPI_MOSI,
        .miso_io_num = RG_GPIO_SDSPI_MISO,
        .sclk_io_num = RG_GPIO_SDSPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    esp_err_t err = spi_bus_initialize(RG_STORAGE_SDSPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) // check but do not abort, let esp_vfs_fat_sdspi_mount decide
        RG_LOGW("SPI bus init failed (0x%x)", err);

    sdmmc_host_t host_config = SDSPI_HOST_DEFAULT();
    host_config.slot = RG_STORAGE_SDSPI_HOST;
    host_config.max_freq_khz = RG_STORAGE_SDSPI_SPEED;
    host_config.do_transaction = &sdcard_do_transaction;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = RG_STORAGE_SDSPI_HOST;
    slot_config.gpio_cs = RG_GPIO_SDSPI_CS;

    // If we're using esp-idf >= 5.0 and the SPI bus is not shared, we must keep the SD card selected
    // to work around slow accesses. (https://github.com/espressif/esp-idf/issues/10493)
    #ifdef RG_STORAGE_SDSPI_HOLD_CS
    gpio_set_direction(slot_config.gpio_cs, GPIO_MODE_OUTPUT);
    gpio_set_level(slot_config.gpio_cs, 0);
    slot_config.gpio_cs = GPIO_NUM_NC;
    #endif

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 0,
    };

    err = esp_vfs_fat_sdspi_mount(RG_STORAGE_ROOT, &host_config, &slot_config, &mount_config, &card_handle);
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_CRC)
    {
        RG_LOGW("SD Card mounting failed (0x%x), retrying at lower speed...\n", err);
        host_config.max_freq_khz = SDMMC_FREQ_PROBING;
        err = esp_vfs_fat_sdspi_mount(RG_STORAGE_ROOT, &host_config, &slot_config, &mount_config, &card_handle);
    }
    error_code = (int)err;

#elif defined(RG_STORAGE_SDMMC_HOST)

    RG_LOGI("Looking for SD Card using SDMMC...");

    sdmmc_host_t host_config = SDMMC_HOST_DEFAULT();
    host_config.flags = SDMMC_HOST_FLAG_1BIT;
    host_config.slot = RG_STORAGE_SDMMC_HOST;
    host_config.max_freq_khz = RG_STORAGE_SDMMC_SPEED;
    host_config.do_transaction = &sdcard_do_transaction;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
#if SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = RG_GPIO_SDSPI_CLK;
    slot_config.cmd = RG_GPIO_SDSPI_CMD;
    slot_config.d0 = RG_GPIO_SDSPI_D0;
    // d1 and d3 normally not used in width=1 but sdmmc_host_init_slot saves them, so just in case
    slot_config.d1 = slot_config.d3 = -1;
#endif

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 0,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(RG_STORAGE_ROOT, &host_config, &slot_config, &mount_config, &card_handle);
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_CRC)
    {
        RG_LOGW("SD Card mounting failed (0x%x), retrying at lower speed...\n", err);
        host_config.max_freq_khz = SDMMC_FREQ_PROBING;
        err = esp_vfs_fat_sdmmc_mount(RG_STORAGE_ROOT, &host_config, &slot_config, &mount_config, &card_handle);
    }
    error_code = (int)err;

#elif defined(RG_STORAGE_USBOTG_HOST)

    #warning "USB OTG isn't available on your SOC"
    RG_LOGI("Looking for USB mass storage...");
    error_code = -1;

#elif !defined(RG_STORAGE_FLASH_PARTITION)

    RG_LOGI("Using host (stdlib) for storage.");
    // Maybe we should just check if RG_STORAGE_ROOT exists?
    error_code = 0;

#endif

#if defined(RG_STORAGE_FLASH_PARTITION)

    if (error_code) // only if no previous storage was successfully mounted already
    {
        RG_LOGI("Looking for an internal flash partition labelled '%s' to mount for storage...", RG_STORAGE_FLASH_PARTITION);

        esp_vfs_fat_mount_config_t mount_config = {
            .format_if_mount_failed = true, // if mount failed, it's probably because it's a clean install so the partition hasn't been formatted yet
            .max_files = 4, // must be initialized, otherwise it will be 0, which doesn't make sense, and will trigger an ESP_ERR_NO_MEM error
        };

        esp_err_t err = esp_vfs_fat_spiflash_mount(RG_STORAGE_ROOT, RG_STORAGE_FLASH_PARTITION, &mount_config, &wl_handle);
        error_code = (int)err;
    }

#endif

    disk_mounted = !error_code;

    if (disk_mounted)
    {
        RG_LOGI("Storage mounted at %s.", RG_STORAGE_ROOT);
        migrate_base_path();
    }
    else
        RG_LOGE("Storage mounting failed! err=0x%x", error_code);
}

void rg_storage_deinit(void)
{
    if (!disk_mounted)
        return;

    rg_storage_commit();

    int error_code = 0;

#if defined(RG_STORAGE_SDSPI_HOST) || defined(RG_STORAGE_SDMMC_HOST)
    if (card_handle != NULL)
    {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(RG_STORAGE_ROOT, card_handle);
        card_handle = NULL; // NULL it regardless of success, nothing we can do on errors...
        error_code = (int)err;
    }
#endif

#if defined(RG_STORAGE_FLASH_PARTITION)
    if (wl_handle != WL_INVALID_HANDLE)
    {
        esp_err_t err = esp_vfs_fat_spiflash_unmount(RG_STORAGE_ROOT, wl_handle);
        wl_handle = WL_INVALID_HANDLE;
        error_code = (int)err;
    }
#endif

    if (error_code)
        RG_LOGE("Storage unmounting failed. err=0x%x", error_code);
    else
        RG_LOGI("Storage unmounted.");

    disk_mounted = false;
}

bool rg_storage_ready(void)
{
    return disk_mounted;
}

void rg_storage_commit(void)
{
    if (!disk_mounted)
        return;
    // flush buffers();
}

bool rg_storage_mkdir(const char *dir)
{
    CHECK_PATH(dir);

    if (mkdir(dir, 0777) == 0)
        return true;

    // FIXME: Might want to stat to see if it's a dir
    if (errno == EEXIST)
        return true;

    // Possibly missing some parents, try creating them
    char *temp = strdup(dir);
    for (char *p = temp + strlen(RG_STORAGE_ROOT) + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = 0;
            if (strlen(temp) > 0)
            {
                mkdir(temp, 0777);
            }
            *p = '/';
            while (*(p + 1) == '/')
                p++;
        }
    }
    free(temp);

    // Finally try again
    if (mkdir(dir, 0777) == 0)
        return true;

    return false;
}

static int delete_cb(const rg_scandir_t *file, void *arg)
{
    rg_storage_delete(file->path);
    return RG_SCANDIR_CONTINUE;
}

bool rg_storage_delete(const char *path)
{
    CHECK_PATH(path);

    // Try the fast way first
    if (remove(path) == 0 || rmdir(path) == 0)
        return true;

    // If that fails, it's likely a non-empty directory and we go recursive
    // (errno could confirm but it has proven unreliable across platforms...)
    if (rg_storage_scandir(path, delete_cb, NULL, 0))
        return rmdir(path) == 0;

    return false;
}

rg_stat_t rg_storage_stat(const char *path)
{
    rg_stat_t ret = {0};
    struct stat statbuf;
    if (path && stat(path, &statbuf) == 0)
    {
        ret.basename = rg_basename(path);
        ret.extension = rg_extension(path);
        ret.size = statbuf.st_size;
        ret.mtime = statbuf.st_mtime;
        ret.is_file = S_ISREG(statbuf.st_mode);
        ret.is_dir = S_ISDIR(statbuf.st_mode);
        ret.exists = true;
    }
    return ret;
}

bool rg_storage_exists(const char *path)
{
    CHECK_PATH(path);
    return access(path, F_OK) == 0;
}

bool rg_storage_scandir(const char *path, rg_scandir_cb_t *callback, void *arg, uint32_t flags)
{
    CHECK_PATH(path);
    uint32_t types = flags & (RG_SCANDIR_FILES | RG_SCANDIR_DIRS);
    size_t path_len = strlen(path) + 1;
    struct stat statbuf;
    struct dirent *ent;

    if (path_len > RG_PATH_MAX - 5)
    {
        RG_LOGE("Folder path too long '%s'", path);
        return false;
    }

    DIR *dir = opendir(path);
    if (!dir)
        return false;

    // We allocate on heap in case we go recursive through rg_storage_delete
    rg_scandir_t *result = calloc(1, sizeof(rg_scandir_t));
    if (!result)
    {
        closedir(dir);
        return false;
    }

    strcat(strcpy(result->path, path), "/");
    result->basename = result->path + path_len;
    result->dirname = path;

    while ((ent = readdir(dir)))
    {
        if (ent->d_name[0] == '.' && (!ent->d_name[1] || ent->d_name[1] == '.'))
        {
            // Skip self and parent
            continue;
        }

        if (path_len + strlen(ent->d_name) >= RG_PATH_MAX)
        {
            RG_LOGE("File path too long '%s/%s'", path, ent->d_name);
            continue;
        }

        strcpy((char *)result->basename, ent->d_name);
    #if defined(DT_REG) && defined(DT_DIR)
        result->is_file = ent->d_type == DT_REG;
        result->is_dir = ent->d_type == DT_DIR;
    #else
        result->is_file = 0;
        result->is_dir = 0;
        // We're forced to stat() if the OS doesn't provide type via dirent
        flags |= RG_SCANDIR_STAT;
    #endif

        if ((flags & RG_SCANDIR_STAT) && stat(result->path, &statbuf) == 0)
        {
            result->is_file = S_ISREG(statbuf.st_mode);
            result->is_dir = S_ISDIR(statbuf.st_mode);
            result->size = statbuf.st_size;
            result->mtime = statbuf.st_mtime;
        }

        if ((result->is_dir && types != RG_SCANDIR_FILES) || (result->is_file && types != RG_SCANDIR_DIRS))
        {
            int ret = (callback)(result, arg);

            if (ret == RG_SCANDIR_STOP)
                break;

            if (ret == RG_SCANDIR_SKIP)
                continue;
        }

        if ((flags & RG_SCANDIR_RECURSIVE) && result->is_dir)
        {
            rg_storage_scandir(result->path, callback, arg, flags);
        }
    }

    closedir(dir);
    free(result);

    return true;
}

int64_t rg_storage_get_free_space(const char *path)
{
    // Here we should translate the provided VFS path to the matching filesystem driver and drive
    // But we don't. Instead we just assume it's drive 0 of the fatfs driver. Yay laziness.
#ifdef ESP_PLATFORM
    DWORD nclst;
    FATFS *fatfs;
    if (f_getfree("0:", &nclst, &fatfs) == FR_OK)
    {
        return (int64_t)nclst * fatfs->csize * fatfs->ssize;
    }
#endif

    return -1;
}

int64_t rg_storage_get_total_space(const char *path)
{
    // Same "drive 0" assumption as rg_storage_get_free_space().
#ifdef ESP_PLATFORM
    DWORD nclst;
    FATFS *fatfs;
    if (f_getfree("0:", &nclst, &fatfs) == FR_OK)
    {
        return (int64_t)(fatfs->n_fatent - 2) * fatfs->csize * fatfs->ssize;
    }
#endif

    return -1;
}

bool rg_storage_read_file(const char *path, void **data_out, size_t *data_len, uint32_t flags)
{
    RG_ASSERT_ARG(data_out && data_len);
    CHECK_PATH(path);

    size_t output_buffer_alloc_size;
    size_t output_buffer_size;
    void *output_buffer;
    size_t file_size;

    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        RG_LOGE("Fopen failed (%d): '%s'", errno, path);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (flags & RG_FILE_USER_BUFFER)
    {
        output_buffer_alloc_size = *data_len;
        output_buffer_size = RG_MIN(*data_len, file_size);
        output_buffer = *data_out;
    }
    else
    {
        size_t blocksize = RG_MAX(0x400, (flags & 0xF) * 0x2000);
        output_buffer_alloc_size = (file_size + (blocksize - 1)) & ~(blocksize - 1);
        output_buffer_size = file_size;
        output_buffer = malloc(output_buffer_alloc_size);
    }

    if (!output_buffer)
    {
        RG_LOGE("Memory allocation failed: '%s'", path);
        fclose(fp);
        return false;
    }

    if (!fread(output_buffer, output_buffer_size, 1, fp))
    {
        RG_LOGE("File read failed (%d): '%s'", errno, path);
        fclose(fp);
        if (!(flags & RG_FILE_USER_BUFFER))
            free(output_buffer);
        return false;
    }

    fclose(fp);

    // Wipe the extra allocated space, if any
    if (output_buffer_alloc_size > output_buffer_size)
    {
        memset(output_buffer + output_buffer_size, 0, output_buffer_alloc_size - output_buffer_size);
    }

    *data_out = output_buffer;
    *data_len = output_buffer_size;
    return true;
}

bool rg_storage_write_file(const char *path, const void *data_ptr, size_t data_len, uint32_t flags)
{
    RG_ASSERT_ARG(data_ptr || !data_len);
    CHECK_PATH(path);

    // TODO: If atomic is true we should write to a temp file and only replace the target on success
    FILE *fp = fopen(path, "wb");
    if (!fp)
    {
        RG_LOGE("Fopen failed (%d): '%s'", errno, path);
        return false;
    }

    if (data_len && !fwrite(data_ptr, data_len, 1, fp))
    {
        RG_LOGE("Fwrite failed (%d): '%s'", errno, path);
        fclose(fp);
        return false;
    }

    fclose(fp);
    return true;
}

/**
 * This is a minimal UNZIP implementation that utilizes only the miniz primitives found in ESP32's ROM.
 * I think that we should use miniz' ZIP API instead and bundle miniz with retro-go. But first I need
 * to do some testing to determine if the increased executable size is acceptable...
 */
#if RG_ZIP_SUPPORT

#if defined(ESP_PLATFORM) && ESP_IDF_VERSION_MAJOR < 5
#include <rom/miniz.h>
#else
#include <miniz.h>
#endif

#define ZIP_MAGIC 0x04034b50
typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint16_t compression;
    uint16_t modified_time;
    uint16_t modified_date;
    uint32_t checksum;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_size;
    uint16_t extra_field_size;
    uint8_t filename[226];
    // uint8_t extra_field[];
    // uint8_t compressed_data[];
} zip_header_t;

bool rg_storage_unzip_file(const char *zip_path, const char *filter, void **data_out, size_t *data_len, uint32_t flags)
{
    RG_ASSERT_ARG(data_out && data_len);
    CHECK_PATH(zip_path);

    zip_header_t header = {0};
    int header_pos = 0;

    FILE *fp = fopen(zip_path, "rb");
    if (!fp)
    {
        RG_LOGE("Fopen failed (%d): '%s'", errno, zip_path);
        return false;
    }

    // Very inefficient, we should read a block at a time and search it for a header. But I'm lazy.
    // Thankfully the header is usually found on the very first read :)
    for (header_pos = 0; !feof(fp) && header_pos < 0x10000; ++header_pos)
    {
        fseek(fp, header_pos, SEEK_SET);
        fread(&header, sizeof(header), 1, fp);
        if (header.magic == ZIP_MAGIC)
            break;
    }

    if (header.magic != ZIP_MAGIC)
    {
        RG_LOGE("No valid header found: '%s'", zip_path);
        fclose(fp);
        return false;
    }

    // Zero terminate or truncate filename just in case
    header.filename[RG_MIN(header.filename_size, 225)] = 0;

    RG_LOGI("Found file at %d, name: '%s', size: %d", header_pos, header.filename, (int)header.uncompressed_size);

    size_t stream_offset = header_pos + 30 + header.filename_size + header.extra_field_size;
    size_t stream_remaining = header.compressed_size;
    size_t output_buffer_align = RG_MAX(0x1000, (flags & 0xF) * 0x2000);
    size_t output_buffer_size;
    size_t output_buffer_pos = 0;
    uint8_t *output_buffer = NULL;

    if (flags & RG_FILE_USER_BUFFER)
    {
        output_buffer_size = RG_MIN(*data_len, header.uncompressed_size);
        output_buffer = *data_out;
    }
    else
    {
        output_buffer_size = header.uncompressed_size;
        output_buffer = malloc((output_buffer_size + (output_buffer_align - 1)) & ~(output_buffer_align - 1));
    }

    size_t read_buffer_size = 0x8000;
    uint8_t *read_buffer = malloc(read_buffer_size);
    tinfl_decompressor *decomp = malloc(sizeof(tinfl_decompressor));

    if (!read_buffer || !output_buffer || !decomp)
    {
        RG_LOGE("Memory allocation failed: '%s'", zip_path);
        goto _fail;
    }

    tinfl_status status;
    tinfl_init(decomp);

    do
    {
        size_t input_size = RG_MIN(read_buffer_size, stream_remaining);
        size_t output_size = output_buffer_size - output_buffer_pos;
        if (fseek(fp, stream_offset, SEEK_SET) != 0 || fread(read_buffer, input_size, 1, fp) != 1)
        {
            RG_LOGE("Read error (%d): '%s'", errno, zip_path);
            goto _fail;
        }
        stream_offset += input_size;
        stream_remaining -= input_size;
        status = tinfl_decompress(
            decomp, read_buffer, &input_size, output_buffer, output_buffer + output_buffer_pos, &output_size,
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | (stream_remaining ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        output_buffer_pos += output_size;
    } while (status == TINFL_STATUS_NEEDS_MORE_INPUT);

    // With user-provided buffer we might not reach TINFL_STATUS_DONE, but it doesn't mean we've failed
    if (status < TINFL_STATUS_DONE || output_buffer_pos != output_buffer_size) // (status != TINFL_STATUS_DONE)
    {
        RG_LOGE("Decompression failed (%d): %s", (int)status, zip_path);
        goto _fail;
    }

    free(read_buffer);
    free(decomp);
    fclose(fp);

    *data_out = output_buffer;
    *data_len = output_buffer_size;
    return true;

_fail:
    if (!(flags & RG_FILE_USER_BUFFER))
        free(output_buffer);
    free(read_buffer);
    free(decomp);
    fclose(fp);
    return false;
}
#else
bool rg_storage_unzip_file(const char *zip_path, const char *filter, void **data_out, size_t *data_len, uint32_t flags)
{
    RG_LOGE("ZIP support hasn't been enabled!");
    return false;
}
#endif
