#include "config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

// NVS namespace used for all doorking keys.
static const char *NVS_NAMESPACE = "doorking";

// NVS keys (max 15 chars each, enforced by the NVS library).
static const char *KEY_WIFI_SSID   = "wifi_ssid";
static const char *KEY_WIFI_PSK    = "wifi_psk";
static const char *KEY_AUTH_TOKEN  = "auth_token";
static const char *KEY_PULSE_MS    = "pulse_ms";
static const char *KEY_MIN_SPACING = "min_spacing";
static const char *KEY_TRAVEL_TO   = "travel_to";
static const char *KEY_HOSTNAME    = "hostname";

void config_defaults(doorking_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->pulse_ms            = 500;    // human-press duration for DKS inputs
    cfg->min_cmd_spacing_ms  = 2000;   // throttle between HTTP commands
    cfg->travel_timeout_ms   = 30000;  // generous ceiling for a slide gate
    strncpy(cfg->hostname, "doorking", DOORKING_HOSTNAME_MAX_LEN);
}

// Load a string NVS key into dst, leaving dst unchanged on any error
// (including ESP_ERR_NVS_NOT_FOUND). Ensures dst stays null-terminated.
static void load_string(nvs_handle_t nvs, const char *key,
                        char *dst, size_t dst_size)
{
    size_t required = dst_size;
    esp_err_t err = nvs_get_str(nvs, key, dst, &required);
    if (err != ESP_OK) {
        // Missing / too large / corrupted — leave the default in place.
        return;
    }
    dst[dst_size - 1] = '\0';
}

// Load a uint32 NVS key into *dst, leaving *dst unchanged on any error.
static void load_u32(nvs_handle_t nvs, const char *key, uint32_t *dst)
{
    uint32_t value;
    if (nvs_get_u32(nvs, key, &value) == ESP_OK) {
        *dst = value;
    }
}

esp_err_t config_load(doorking_config_t *cfg)
{
    config_defaults(cfg);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // First boot — namespace has never been written. Defaults are fine.
        ESP_LOGI(TAG, "nvs namespace empty, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    load_string(nvs, KEY_WIFI_SSID,  cfg->wifi_ssid,  sizeof(cfg->wifi_ssid));
    load_string(nvs, KEY_WIFI_PSK,   cfg->wifi_psk,   sizeof(cfg->wifi_psk));
    load_string(nvs, KEY_AUTH_TOKEN, cfg->auth_token, sizeof(cfg->auth_token));
    load_string(nvs, KEY_HOSTNAME,   cfg->hostname,   sizeof(cfg->hostname));

    load_u32(nvs, KEY_PULSE_MS,    &cfg->pulse_ms);
    load_u32(nvs, KEY_MIN_SPACING, &cfg->min_cmd_spacing_ms);
    load_u32(nvs, KEY_TRAVEL_TO,   &cfg->travel_timeout_ms);

    nvs_close(nvs);
    return ESP_OK;
}

#define SAVE_OR_FAIL(fn_call) do { \
    esp_err_t _e = (fn_call); \
    if (_e != ESP_OK) { \
        ESP_LOGE(TAG, "nvs write failed (%s): %s", #fn_call, esp_err_to_name(_e)); \
        nvs_close(nvs); \
        return _e; \
    } \
} while (0)

esp_err_t config_save(const doorking_config_t *cfg)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw failed: %s", esp_err_to_name(err));
        return err;
    }

    SAVE_OR_FAIL(nvs_set_str(nvs, KEY_WIFI_SSID,   cfg->wifi_ssid));
    SAVE_OR_FAIL(nvs_set_str(nvs, KEY_WIFI_PSK,    cfg->wifi_psk));
    SAVE_OR_FAIL(nvs_set_str(nvs, KEY_AUTH_TOKEN,  cfg->auth_token));
    SAVE_OR_FAIL(nvs_set_str(nvs, KEY_HOSTNAME,    cfg->hostname));
    SAVE_OR_FAIL(nvs_set_u32(nvs, KEY_PULSE_MS,    cfg->pulse_ms));
    SAVE_OR_FAIL(nvs_set_u32(nvs, KEY_MIN_SPACING, cfg->min_cmd_spacing_ms));
    SAVE_OR_FAIL(nvs_set_u32(nvs, KEY_TRAVEL_TO,   cfg->travel_timeout_ms));

    SAVE_OR_FAIL(nvs_commit(nvs));
    nvs_close(nvs);
    ESP_LOGI(TAG, "config saved");
    return ESP_OK;
}

esp_err_t config_clear_wifi(void)
{
    doorking_config_t cfg;
    esp_err_t err = config_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    cfg.wifi_ssid[0]  = '\0';
    cfg.wifi_psk[0]   = '\0';
    cfg.auth_token[0] = '\0';
    err = config_save(&cfg);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "wifi credentials and auth token cleared");
    }
    return err;
}

bool config_has_wifi(const doorking_config_t *cfg)
{
    return cfg->wifi_ssid[0] != '\0';
}

bool config_has_auth_token(const doorking_config_t *cfg)
{
    return cfg->auth_token[0] != '\0';
}

void config_log(const doorking_config_t *cfg)
{
    ESP_LOGI(TAG, "config: hostname=%s", cfg->hostname);
    ESP_LOGI(TAG, "config: wifi_ssid=%s psk=%s",
             config_has_wifi(cfg) ? cfg->wifi_ssid : "(unset)",
             config_has_wifi(cfg) ? "<set>" : "(unset)");

    // The bearer token is logged in full at every boot by design. Seeing it
    // is gated on physical USB access to the XIAO, which is the same trust
    // level as physical access to the gate controller itself, so logging
    // here is no worse than the "erase flash + re-provision" recovery path.
    // It is also the primary delivery channel if the provisioning page
    // fails to render on the user's phone (iOS captive-portal view is
    // fragile about this). Tighten this if the threat model changes.
    if (config_has_auth_token(cfg)) {
        ESP_LOGI(TAG, "config: auth_token=%s", cfg->auth_token);
    } else {
        ESP_LOGI(TAG, "config: auth_token=(unset)");
    }

    ESP_LOGI(TAG, "config: pulse_ms=%u min_cmd_spacing_ms=%u travel_timeout_ms=%u",
             (unsigned)cfg->pulse_ms,
             (unsigned)cfg->min_cmd_spacing_ms,
             (unsigned)cfg->travel_timeout_ms);
}
