// OTA update support: rollback confirmation, push endpoint (POST /update),
// and pull-based auto-check against GitHub Releases.

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Confirm the running firmware is valid (rollback protection) and start
// the periodic GitHub Release check timer. Call from app_main after
// WiFi + HTTP are up.
void ota_init(void);

// Write a firmware image to the next OTA partition and reboot.
// Called from the POST /update HTTP handler (auth is checked by the
// caller in http_api.c). Reads the binary body from req in chunks.
esp_err_t ota_push_from_http(httpd_req_t *req);

// Return the latest available version from the most recent GitHub check,
// or NULL if no update is available / the check hasn't run yet. The
// pointer is to a static buffer valid until the next check overwrites it.
const char *ota_get_available_version(void);

// Check GitHub for a new release right now (no install). Updates the
// cached available version. Spawns a one-shot FreeRTOS task and returns
// immediately. Returns ESP_ERR_INVALID_STATE if a check is in progress.
esp_err_t ota_check_now(void);

// Trigger a pull-based OTA right now (download from GitHub + flash).
// Spawns a one-shot FreeRTOS task and returns immediately.
// Returns ESP_ERR_INVALID_STATE if an OTA is already in progress.
esp_err_t ota_pull_now(void);

// Current OTA state — surfaced via GET /update/status so the dashboard
// can show progress and errors without the user having to tail logs.
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_FAILED,     // last operation failed; last_error is populated
} ota_state_t;

ota_state_t  ota_get_state(void);
const char  *ota_state_name(ota_state_t s);

// Human-readable error message from the most recent failure, or empty
// string if state is not FAILED. Points to a static buffer that is
// overwritten by the next operation.
const char  *ota_get_last_error(void);

#ifdef __cplusplus
}
#endif
