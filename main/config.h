// Persistent configuration for the doorking bridge.
//
// Values live in NVS under the "doorking" namespace. config_load fills in
// defaults for any key that is missing, so a blank device behaves
// identically to a fresh install. The caller mutates the struct in memory
// and calls config_save to persist the whole thing atomically.
//
// Secrets (wifi_psk, auth_token) are NEVER logged by this module.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Max WiFi SSID length per 802.11 is 32 bytes; PSK is 64 bytes max.
// We store both null-terminated for ease of use with esp_wifi_set_config.
#define DOORKING_WIFI_SSID_MAX_LEN   32
#define DOORKING_WIFI_PSK_MAX_LEN    64

// Bearer token: we generate 32 random bytes and base64-encode (44 chars).
// Leave headroom for manually-set tokens.
#define DOORKING_AUTH_TOKEN_MAX_LEN  64

// Hostname (used for mDNS and the AP SSID when unprovisioned).
#define DOORKING_HOSTNAME_MAX_LEN    32

typedef struct {
    // WiFi station credentials. Both empty means "not provisioned yet";
    // the firmware falls back to AP mode in that case.
    char wifi_ssid[DOORKING_WIFI_SSID_MAX_LEN + 1];
    char wifi_psk[DOORKING_WIFI_PSK_MAX_LEN + 1];

    // HTTP bearer token. Empty means "not provisioned"; the HTTP layer
    // refuses all auth'd requests until one is set.
    char auth_token[DOORKING_AUTH_TOKEN_MAX_LEN + 1];

    // Gate state-machine timings (milliseconds). These feed directly into
    // gate_sm_config_t at boot.
    uint32_t pulse_ms;
    uint32_t min_cmd_spacing_ms;
    uint32_t travel_timeout_ms;

    // Network identity.
    char hostname[DOORKING_HOSTNAME_MAX_LEN + 1];

    // OTA settings.
    uint8_t  ota_auto_check;       // 1 = poll GitHub for updates (default)
    uint8_t  ota_auto_install;     // 1 = auto-install + reboot (default 0)
    uint32_t ota_check_interval_s; // seconds between checks (default 21600 = 6 h)
} doorking_config_t;

// Populate cfg with compile-time defaults. Does not touch NVS.
void config_defaults(doorking_config_t *cfg);

// Load config from NVS, filling in defaults for any missing key. Safe to
// call on a blank device (returns ESP_OK with defaults). Call nvs_flash_init
// before this.
esp_err_t config_load(doorking_config_t *cfg);

// Persist the whole cfg struct to NVS in one transaction.
esp_err_t config_save(const doorking_config_t *cfg);

// Zero out wifi_ssid, wifi_psk, and auth_token in NVS so the next boot
// falls back to AP provisioning mode. Gate timings and hostname are
// preserved. Used by the reset-button path.
esp_err_t config_clear_wifi(void);

// True if wifi_ssid is non-empty (i.e. station-mode provisioning has been
// completed at least once).
bool config_has_wifi(const doorking_config_t *cfg);

// True if auth_token is non-empty.
bool config_has_auth_token(const doorking_config_t *cfg);

// Log the non-secret fields of cfg at INFO level. Never logs wifi_psk or
// auth_token (only whether they are set).
void config_log(const doorking_config_t *cfg);

#ifdef __cplusplus
}
#endif
