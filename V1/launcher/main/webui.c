#include "rg_system.h"
#include "gui.h"
#include "applications.h"

#ifdef RG_ENABLE_NETWORKING
#include <esp_http_server.h>
#include <dirent.h>
#include <stdio.h>
#include <cJSON.h>
#include <ctype.h>
#include <mbedtls/base64.h>
#include <esp_mac.h>

// static const char webui_html[];
#include "webui.html.h"

static httpd_handle_t server;
static char *http_buffer;

// The server hands out read/write access to the whole SD card, including the
// stored Wi-Fi passwords in the settings, so it asks for a password. This is
// HTTP Basic auth over plain HTTP: the password is only base64-encoded on the
// wire, so it keeps other people on the same network out, but it is not
// protection against someone actually sniffing the traffic. Real transport
// security would need TLS, which isn't worth the flash here.
#define SETTING_WEBUI_PASS "WebUIPassword"

static char webui_password[33];

const char *webui_default_password(void)
{
    // Per device rather than one shared word: with the source public, a fixed
    // default is no secret at all. The MAC is unique and already printed by the
    // "SD card in browser" dialog, so nobody has to remember anything.
    static char fallback[16];
    if (!fallback[0])
    {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(fallback, sizeof(fallback), "bb-%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    return fallback;
}

void webui_reload_password(void)
{
    char *stored = rg_settings_get_string(NS_APP, SETTING_WEBUI_PASS,
                                          webui_default_password());
    snprintf(webui_password, sizeof(webui_password), "%s", stored ? stored : "");
    free(stored);
}

// The browser prompts for a user AND a password, but only the password is
// checked -- one secret is enough here, and it saves typing a username on a
// d-pad keyboard. An empty password disables the check (open server).
static bool webui_authorized(httpd_req_t *req)
{
    if (!webui_password[0])
        return true;

    char header[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK)
        return false;
    if (strncmp(header, "Basic ", 6) != 0)
        return false;

    unsigned char decoded[128];
    size_t len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &len,
                              (const unsigned char *)header + 6, strlen(header + 6)) != 0)
        return false;
    decoded[len] = 0;

    // "user:password" -- everything after the first colon is the password.
    char *sep = strchr((char *)decoded, ':');
    return strcmp(sep ? sep + 1 : (char *)decoded, webui_password) == 0;
}

static esp_err_t webui_deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    // The realm text is shown by the browser inside its own login dialog. Basic
    // auth always renders a username field even though we only check the
    // password, so say so here -- otherwise the user is left guessing.
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       "Basic realm=\"BlockBoy - user: blockboy\"");
    httpd_resp_sendstr(req, "Password required.");
    return ESP_OK;
}

static char *urldecode(const char *str)
{
    char *new_string = strdup(str);
    char *ptr = new_string;
    while (ptr[0] && ptr[1] && ptr[2])
    {
        if (ptr[0] == '%' && isxdigit((unsigned char)ptr[1]) && isxdigit((unsigned char)ptr[2]))
        {
            char hex[] = {ptr[1], ptr[2], 0};
            *ptr = strtol(hex, NULL, 16);
            memmove(ptr + 1, ptr + 3, strlen(ptr + 3) + 1);
        }
        ptr++;
    }
    return new_string;
}

static int add_file(const rg_scandir_t *entry, void *arg)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", entry->basename);
    cJSON_AddNumberToObject(obj, "size", entry->size);
    cJSON_AddNumberToObject(obj, "mtime", entry->mtime);
    // cJSON_AddBoolToObject(obj, "is_file", entry->is_file);
    cJSON_AddBoolToObject(obj, "is_dir", entry->is_dir);
    cJSON_AddItemToArray((cJSON *)arg, obj);
    return RG_SCANDIR_CONTINUE;
}

static esp_err_t http_api_handler(httpd_req_t *req)
{
    if (!webui_authorized(req))
        return webui_deny(req);

    char http_buffer[1024] = {0};
    bool success = false;
    FILE *fp;

    if (httpd_req_recv(req, http_buffer, sizeof(http_buffer)) < 2)
        return ESP_FAIL;

    cJSON *content = cJSON_Parse(http_buffer);
    if (!content)
        return ESP_FAIL;

    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(content, "cmd")) ?: "-";
    const char *arg1 = cJSON_GetStringValue(cJSON_GetObjectItem(content, "arg1")) ?: "";
    const char *arg2 = cJSON_GetStringValue(cJSON_GetObjectItem(content, "arg2")) ?: "";

    cJSON *response = cJSON_CreateObject();

    gui.http_lock = true;

    if (strcmp(cmd, "list") == 0)
    {
        cJSON *array = cJSON_AddArrayToObject(response, "files");
        success = array && rg_storage_scandir(arg1, add_file, array, RG_SCANDIR_SORT | RG_SCANDIR_STAT);
    }
    else if (strcmp(cmd, "rename") == 0)
    {
        success = rename(arg1, arg2) == 0;
        gui_invalidate();
    }
    else if (strcmp(cmd, "delete") == 0)
    {
        success = rg_storage_delete(arg1);
        gui_invalidate();
    }
    else if (strcmp(cmd, "mkdir") == 0)
    {
        success = rg_storage_mkdir(arg1);
        gui_invalidate();
    }
    else if (strcmp(cmd, "touch") == 0)
    {
        success = (fp = fopen(arg1, "wb")) && fclose(fp) == 0;
        gui_invalidate();
    }
    else if (strcmp(cmd, "stat") == 0)
    {
        // Device status for the web UI header: battery, firmware version, SD space.
        rg_battery_t batt = rg_input_read_battery();
        cJSON_AddNumberToObject(response, "battery", batt.present ? batt.level : -1);
        cJSON_AddBoolToObject(response, "charging", batt.charging);
        cJSON_AddStringToObject(response, "version", rg_system_get_app()->version ?: "");
        cJSON_AddNumberToObject(response, "free", (double)rg_storage_get_free_space(RG_STORAGE_ROOT));
        cJSON_AddNumberToObject(response, "total", (double)rg_storage_get_total_space(RG_STORAGE_ROOT));
        success = true;
    }
    else if (strcmp(cmd, "play") == 0)
    {
        // Launch a ROM. The app switch calls shutdown_cleanup() (heavy display/SPI
        // work), so it must NOT run in this HTTP task concurrently with the main
        // GUI task -- that races the SPI bus and crashes. Instead we validate the
        // path here and hand it to the main loop, which performs the switch from
        // its own context (exactly like a normal launch).
        retro_file_t target;
        if (rg_storage_exists(arg1) && application_path_to_file(arg1, &target))
        {
            success = true;
            if (!gui.pending_launch)
                gui.pending_launch = strdup(arg1);
            free(target.name); // target.folder is a pooled string, don't free it
        }
    }

    gui.http_lock = false;

    cJSON_AddBoolToObject(response, "success", success);

    char *response_text = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response_text);
    free(response_text);

    cJSON_Delete(response);
    cJSON_Delete(content);

    return ESP_OK;
}

static esp_err_t http_upload_handler(httpd_req_t *req)
{
    if (!webui_authorized(req))
        return webui_deny(req);

    char *filename = urldecode(req->uri);
    bool success = false;
    RG_LOGI("Receiving file: %s", filename);

    gui.http_lock = true;
    rg_task_delay(100);

    FILE *fp = fopen(filename, "wb");
    if (!fp)
        goto _done;

    size_t received = 0;

    while (received < req->content_len)
    {
        int length = httpd_req_recv(req, http_buffer, 0x8000);
        if (length <= 0)
            break;
        if (!fwrite(http_buffer, length, 1, fp))
        {
            RG_LOGE("Write failure at %d bytes", received);
            break;
        }
        received += length;
        rg_task_yield();
    }

    fclose(fp);

    RG_LOGI("Received %d/%d bytes", received, req->content_len);
    success = received == req->content_len;

    gui.http_lock = false;
    gui_invalidate();

_done:
    if (!success)
    {
        RG_LOGE("File receive error!");
        httpd_resp_sendstr(req, "ERROR");
        remove(filename);
        free(filename);
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "OK");
    free(filename);
    return ESP_OK;
}

static esp_err_t http_download_handler(httpd_req_t *req)
{
    if (!webui_authorized(req))
        return webui_deny(req);

    char *filename = urldecode(req->uri);
    FILE *fp;

    RG_LOGI("Serving file: %s", filename);

    gui.http_lock = true;

    if ((fp = fopen(filename, "rb")))
    {
        if (rg_extension_match(filename, "json log txt"))
            httpd_resp_set_type(req, "text/plain");
        else if (rg_extension_match(filename, "png") == 0)
            httpd_resp_set_type(req, "image/png");
        else if (rg_extension_match(filename, "jpg") == 0)
            httpd_resp_set_type(req, "image/jpg");
        else
            httpd_resp_set_type(req, "application/binary");

        for (size_t len; (len = fread(http_buffer, 1, 0x8000, fp));)
        {
            httpd_resp_send_chunk(req, http_buffer, len);
            rg_task_yield();
        }

        httpd_resp_send_chunk(req, NULL, 0);
        fclose(fp);
    }
    else
    {
        httpd_resp_send_404(req);
    }
    free(filename);

    gui.http_lock = false;

    return ESP_OK;
}

static esp_err_t http_get_handler(httpd_req_t *req)
{
    if (!webui_authorized(req))
        return webui_deny(req);

    // No caching: the UI is baked into each firmware build, so a browser must
    // always fetch the current version instead of a stale cached copy.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, must-revalidate");
    httpd_resp_sendstr(req, webui_html);
    return ESP_OK;
}

bool webui_running(void)
{
    return server != NULL;
}

void webui_stop(void)
{
    if (!server) // Already stopped
        return;

    httpd_stop(server);
    server = NULL;

    free(http_buffer);
    http_buffer = NULL;
}

bool webui_start(void)
{
    if (server) // Already started
        return true;

    webui_reload_password();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK)
    {
        RG_LOGE("Failed to start webserver: 0x%03X", err);
        server = NULL;
        return false;
    }

    // 64KB transfer buffer. malloc can fail on a fragmented heap, so unwind
    // cleanly instead of asserting -- a file server that won't start must never
    // take the whole device down.
    http_buffer = malloc(0x10000);
    if (!http_buffer)
    {
        RG_LOGE("Failed to allocate the 64KB transfer buffer");
        httpd_stop(server);
        server = NULL;
        return false;
    }

    httpd_register_uri_handler(server, &(httpd_uri_t){
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = http_get_handler,
    });

    httpd_register_uri_handler(server, &(httpd_uri_t){
        .uri       = "/api",
        .method    = HTTP_POST,
        .handler   = http_api_handler,
    });

    httpd_register_uri_handler(server, &(httpd_uri_t){
        .uri       = "/*",
        .method    = HTTP_GET,
        .handler   = http_download_handler,
    });

    httpd_register_uri_handler(server, &(httpd_uri_t){
        .uri       = "/*",
        .method    = HTTP_PUT,
        .handler   = http_upload_handler,
    });

    RG_LOGI("Web server started");
    return true;
}

#endif
