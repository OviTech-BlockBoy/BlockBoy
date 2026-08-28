#include "rg_system.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <cJSON.h>

static cJSON *config_root = NULL;
static bool safe_mode = false;

// The config tree is reached from more than one task, and they run on different
// cores: the main task builds it in rg_settings_init() while the input task
// (pinned to core 1, see rg_input.c) already polls the battery, which reads
// "BattCalPct" through rg_input_get_battery_cal(). cJSON's insert walks a linked
// list and is not atomic, so without this lock the two can interleave -- most
// destructively while rg_settings_init() is still waiting on the SD read: the
// other core then finds the namespace but no "values" yet, creates an empty one,
// and the freshly parsed object gets appended behind it. cJSON_GetObjectItem()
// returns the first match, so every setting in the file becomes invisible and the
// empty object is what gets written back. Symptom: settings silently reset on
// every boot, moving around with the memory layout and timing.
static rg_mutex_t *settings_lock = NULL;

// Returns false when settings aren't up yet (config_root is NULL then, so the
// callers below correctly fall back to their default values).
static bool lock_settings(void)
{
    if (!settings_lock)
        return false;
    if (!rg_mutex_take(settings_lock, 10000))
    {
        RG_LOGE("Failed to acquire settings lock!");
        return false;
    }
    return true;
}

#define UNLOCK_SETTINGS() rg_mutex_give(settings_lock)

// Callers must hold settings_lock.
static cJSON *json_root(const char *name, bool set_dirty)
{
    if (!config_root)
        return NULL;

    if (name == NS_GLOBAL)
        name = "global";
    else if (name == NS_APP)
        name = rg_system_get_app()->configNs;
    else if (name == NS_FILE)
        name = rg_basename(rg_system_get_app()->romPath);
    else if (name == NS_WIFI)
        name = "wifi";
    else if (name == NS_BOOT)
        name = "boot";

    cJSON *branch = cJSON_GetObjectItem(config_root, name);
    if (!branch)
    {
        branch = cJSON_AddObjectToObject(config_root, name);
        cJSON_AddStringToObject(branch, "namespace", name);
        cJSON_AddNumberToObject(branch, "changed", 0);
        if (!safe_mode)
        {
            char *data; size_t data_len;
            char pathbuf[RG_PATH_MAX];
            snprintf(pathbuf, RG_PATH_MAX, "%s/%s.json", RG_BASE_PATH_CONFIG, name);
            if (rg_storage_read_file(pathbuf, (void **)&data, &data_len, 0))
            {
                cJSON *values = cJSON_Parse(data);
                if (!values) // Parse failure, clean the markup and try again
                    values = cJSON_Parse(rg_json_fixup(data));
                if (values)
                {
                    RG_LOGI("Config file loaded: '%s'", pathbuf);
                    cJSON_AddItemToObject(branch, "values", values);
                }
                else
                    RG_LOGE("Config file parsing failed: '%s'", pathbuf);
                free(data);
            }
        }
    }

    cJSON *root = cJSON_GetObjectItem(branch, "values");
    if (!cJSON_IsObject(root))
    {
        cJSON_DeleteItemFromObject(branch, "values");
        root = cJSON_AddObjectToObject(branch, "values");
    }

    if (set_dirty)
    {
        cJSON_SetNumberHelper(cJSON_GetObjectItem(branch, "changed"), 1);
    }

    return root;
}

// Callers must hold settings_lock.
static void update_value(const char *section, const char *key, cJSON *new_value)
{
    cJSON *obj = cJSON_GetObjectItem(json_root(section, 0), key);
    if (obj == NULL)
        cJSON_AddItemToObject(json_root(section, 1), key, new_value);
    else if (!cJSON_Compare(obj, new_value, true))
        cJSON_ReplaceItemInObject(json_root(section, 1), key, new_value);
    else
        cJSON_Delete(new_value);
}

void rg_settings_init(bool _safe_mode)
{
    // Create the lock before config_root exists: until then json_root() returns
    // NULL and every reader falls back to its default, which is safe.
    if (!settings_lock)
        settings_lock = rg_mutex_create();
    if (!lock_settings())
        return;
    config_root = cJSON_CreateObject();
    safe_mode = _safe_mode;
    json_root(NS_GLOBAL, 0);
    json_root(NS_BOOT, 0);
    UNLOCK_SETTINGS();
}

void rg_settings_commit(void)
{
    if (!lock_settings())
        return;

    if (!config_root || safe_mode)
    {
        UNLOCK_SETTINGS();
        return;
    }

    for (cJSON *branch = config_root->child; branch; branch = branch->next)
    {
        char *name = cJSON_GetStringValue(cJSON_GetObjectItem(branch, "namespace"));
        int changed = cJSON_GetNumberValue(cJSON_GetObjectItem(branch, "changed"));

        if (!changed)
            continue;

        char *buffer = cJSON_Print(cJSON_GetObjectItem(branch, "values"));
        if (!buffer)
            continue;

        size_t buffer_len = strlen(buffer) + 1;

        char pathbuf[RG_PATH_MAX];
        snprintf(pathbuf, RG_PATH_MAX, "%s/%s.json", RG_BASE_PATH_CONFIG, name);
        if (rg_storage_write_file(pathbuf, buffer, buffer_len, 0) ||
            (rg_storage_mkdir(rg_dirname(pathbuf)) && rg_storage_write_file(pathbuf, buffer, buffer_len, 0)))
        {
            cJSON_SetNumberHelper(cJSON_GetObjectItem(branch, "changed"), 0);
        }
        else
            RG_LOGE("Failed to write config file: '%s'", pathbuf);

        cJSON_free(buffer);
    }

    rg_storage_commit();
    UNLOCK_SETTINGS();
}

void rg_settings_reset(void)
{
    RG_LOGI("Clearing settings...\n");
    rg_storage_delete(RG_BASE_PATH_CONFIG);
    rg_storage_mkdir(RG_BASE_PATH_CONFIG);
    if (!lock_settings())
        return;
    if (config_root)
    {
        cJSON_Delete(config_root);
        config_root = cJSON_CreateObject();
    }
    UNLOCK_SETTINGS();
}

bool rg_settings_get_boolean(const char *section, const char *key, bool default_value)
{
    if (!lock_settings())
        return default_value;
    cJSON *obj = cJSON_GetObjectItem(json_root(section, 0), key);
    bool value = cJSON_IsNumber(obj) // Backwards compatible with plain numbers
                     ? (obj->valueint != 0)
                     : (cJSON_IsBool(obj) ? cJSON_IsTrue(obj) : default_value);
    UNLOCK_SETTINGS();
    return value;
}

void rg_settings_set_boolean(const char *section, const char *key, bool value)
{
    if (!lock_settings())
        return;
    update_value(section, key, cJSON_CreateBool(value));
    UNLOCK_SETTINGS();
}

double rg_settings_get_number(const char *section, const char *key, double default_value)
{
    if (!lock_settings())
        return default_value;
    cJSON *obj = cJSON_GetObjectItem(json_root(section, 0), key);
    double value = cJSON_IsNumber(obj) ? obj->valuedouble : default_value;
    UNLOCK_SETTINGS();
    return value;
}

void rg_settings_set_number(const char *section, const char *key, double value)
{
    if (!lock_settings())
        return;
    update_value(section, key, cJSON_CreateNumber(value));
    UNLOCK_SETTINGS();
}

char *rg_settings_get_string(const char *section, const char *key, const char *default_value)
{
    if (!lock_settings())
        return default_value ? strdup(default_value) : NULL;
    cJSON *obj = cJSON_GetObjectItem(json_root(section, 0), key);
    char *value = cJSON_IsString(obj) ? strdup(obj->valuestring)
                                      : (default_value ? strdup(default_value) : NULL);
    UNLOCK_SETTINGS();
    return value;
}

void rg_settings_set_string(const char *section, const char *key, const char *value)
{
    if (!lock_settings())
        return;
    update_value(section, key, value ? cJSON_CreateString(value) : cJSON_CreateNull());
    UNLOCK_SETTINGS();
}

void rg_settings_delete(const char *section, const char *key)
{
    if (!lock_settings())
        return;
    cJSON_DeleteItemFromObject(json_root(section, 1), key);
    UNLOCK_SETTINGS();
}

bool rg_settings_exists(const char *section, const char *key)
{
    if (!lock_settings())
        return false;
    bool exists = cJSON_GetObjectItem(json_root(section, 0), key) != NULL;
    UNLOCK_SETTINGS();
    return exists;
}
