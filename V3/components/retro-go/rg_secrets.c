#include "rg_system.h"
#include "rg_secrets.h"
#include "rg_settings.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include <nvs.h>
#include <nvs_flash.h>

#define SECRETS_NAMESPACE "blockboy"

// nvs_flash_init is also called by the Wi-Fi and Bluetooth stacks, and calling
// it twice is harmless, so this just makes sure it has happened before the
// first secret is read -- which can be well before the radio is switched on.
static bool nvs_ready(void)
{
    static bool done = false;

    if (!done)
    {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            // A partition from an older layout, or a half-written one. Wiping it
            // loses the stored secrets, which means the user re-enters a Wi-Fi
            // password; leaving it broken would mean no secrets at all.
            RG_LOGW("NVS needs erasing (0x%X), stored secrets will be lost\n", err);
            if (nvs_flash_erase() == ESP_OK)
                err = nvs_flash_init();
        }
        if (err != ESP_OK)
        {
            RG_LOGE("nvs_flash_init failed: 0x%X\n", err);
            return false;
        }
        done = true;
    }

    return done;
}

bool rg_secret_get(const char *key, char *out, size_t out_len)
{
    RG_ASSERT_ARG(key && out && out_len);
    out[0] = 0;

    if (!nvs_ready())
        return false;

    nvs_handle_t handle;
    if (nvs_open(SECRETS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return false; // namespace does not exist yet: nothing stored

    size_t length = out_len;
    esp_err_t err = nvs_get_str(handle, key, out, &length);
    nvs_close(handle);

    if (err != ESP_OK)
    {
        out[0] = 0;
        return false;
    }

    return out[0] != 0;
}

bool rg_secret_set(const char *key, const char *value)
{
    RG_ASSERT_ARG(key && value);

    if (!nvs_ready())
        return false;

    nvs_handle_t handle;
    if (nvs_open(SECRETS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;

    esp_err_t err = nvs_set_str(handle, key, value);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK)
        RG_LOGE("Could not store secret '%s': 0x%X\n", key, err);

    return err == ESP_OK;
}

bool rg_secret_delete(const char *key)
{
    RG_ASSERT_ARG(key);

    if (!nvs_ready())
        return false;

    nvs_handle_t handle;
    if (nvs_open(SECRETS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return false;

    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);

    // Not being there is the state the caller wanted anyway.
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

#else // host build (SDL): no NVS, fall back to the settings file

bool rg_secret_get(const char *key, char *out, size_t out_len)
{
    RG_ASSERT_ARG(key && out && out_len);
    out[0] = 0;
    char *stored = rg_settings_get_string(NS_GLOBAL, key, NULL);
    if (stored)
        snprintf(out, out_len, "%s", stored);
    free(stored);
    return out[0] != 0;
}

bool rg_secret_set(const char *key, const char *value)
{
    rg_settings_set_string(NS_GLOBAL, key, value);
    return true;
}

bool rg_secret_delete(const char *key)
{
    rg_settings_delete(NS_GLOBAL, key);
    return true;
}

#endif

void rg_secret_migrate(const char *section, const char *settings_key, const char *secret_key)
{
    RG_ASSERT_ARG(settings_key && secret_key);

    char *old = rg_settings_get_string(section, settings_key, NULL);
    if (!old)
        return; // nothing on the card, nothing to do

    // Fill NVS only when it is still empty. A value that is already there wins
    // over whatever is left on the card: the card may have been in another
    // device, or be an old copy.
    char existing[96];
    if (!rg_secret_get(secret_key, existing, sizeof(existing)) && old[0])
    {
        if (rg_secret_set(secret_key, old))
            RG_LOGI("Moved '%s' out of the config file into NVS\n", settings_key);
    }

    // Removed either way. An empty leftover is not worth keeping, and a value we
    // failed to store is not worth leaving on a removable card. Committed right
    // here: this runs once in the lifetime of a card, and leaving the plaintext
    // behind until some later save happens to flush is the thing we are fixing.
    rg_settings_delete(section, settings_key);
    rg_settings_commit();
    free(old);
}
