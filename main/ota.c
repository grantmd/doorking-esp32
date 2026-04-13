#include "ota.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_types.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "status_led.h"

static const char *TAG = "ota";

// ---------------------------------------------------------------------------
// Cached update state — written by the check task, read by /health and GET /
// ---------------------------------------------------------------------------

static char s_available_version[32];  // e.g. "0.5.0", or empty if none
static char s_asset_url[512];         // download URL for the matching binary

const char *ota_get_available_version(void)
{
    if (s_available_version[0] == '\0') {
        return NULL;
    }
    return s_available_version;
}

// ---------------------------------------------------------------------------
// Rollback confirmation
// ---------------------------------------------------------------------------

static esp_timer_handle_t s_rollback_timer;

static void rollback_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "rollback timer fired — no IP within 120 s, restarting");
    esp_restart();
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;

    // Confirm the running firmware and cancel the watchdog.
    esp_ota_mark_app_valid_cancel_rollback();
    if (s_rollback_timer) {
        esp_timer_stop(s_rollback_timer);
        esp_timer_delete(s_rollback_timer);
        s_rollback_timer = NULL;
    }
    ESP_LOGI(TAG, "firmware confirmed valid (rollback cancelled)");
}

static void rollback_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "running image is PENDING_VERIFY — starting 120 s watchdog");

        const esp_timer_create_args_t args = {
            .callback = rollback_timer_cb,
            .name = "ota_rollback",
        };
        esp_timer_create(&args, &s_rollback_timer);
        esp_timer_start_once(s_rollback_timer, 120ULL * 1000 * 1000);

        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   on_got_ip, NULL);
    } else {
        // Normal boot or first-ever flash — nothing to confirm.
        ESP_LOGI(TAG, "firmware not pending verify, skipping rollback watchdog");
    }
}

// ---------------------------------------------------------------------------
// Delayed reboot — lets the HTTP response flush before restart
// ---------------------------------------------------------------------------

static void delayed_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

// ---------------------------------------------------------------------------
// Push OTA — POST /update
// ---------------------------------------------------------------------------

esp_err_t ota_push_from_http(httpd_req_t *req)
{

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"no OTA partition\"}");
    }

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"ota_begin failed\"}");
    }

    status_led_set_state(STATUS_LED_OTA_IN_PROGRESS);

    char buf[4096];
    int remaining = req->content_len;
    int total = 0;

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, sizeof(buf));
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;  // retry on timeout
            }
            ESP_LOGE(TAG, "recv error during OTA push");
            esp_ota_abort(handle);
            status_led_set_state(STATUS_LED_WIFI_CONNECTED);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"error\":\"recv failed\"}");
        }

        err = esp_ota_write(handle, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            status_led_set_state(STATUS_LED_WIFI_CONNECTED);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"error\":\"ota_write failed\"}");
        }

        total += recv_len;
        remaining -= recv_len;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s (image invalid?)", esp_err_to_name(err));
        status_led_set_state(STATUS_LED_WIFI_CONNECTED);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"image validation failed\"}");
    }

    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        status_led_set_state(STATUS_LED_WIFI_CONNECTED);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"error\":\"set_boot_partition failed\"}");
    }

    ESP_LOGI(TAG, "OTA push complete (%d bytes), rebooting in 1 s", total);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"result\":\"ok\",\"rebooting\":true}");

    // Reboot in a separate task so the HTTP response flushes first.
    xTaskCreate(delayed_reboot_task, "ota_reboot", 2048, NULL, 5, NULL);

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Semver comparison — returns >0 if a > b, 0 if equal, <0 if a < b
// ---------------------------------------------------------------------------

static int parse_semver(const char *s, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    // Skip leading 'v' if present.
    if (*s == 'v' || *s == 'V') {
        s++;
    }
    int n = sscanf(s, "%d.%d.%d", &out[0], &out[1], &out[2]);
    return n >= 2 ? 0 : -1;  // need at least major.minor
}

static int semver_cmp(const char *a, const char *b)
{
    int va[3], vb[3];
    if (parse_semver(a, va) != 0 || parse_semver(b, vb) != 0) {
        return 0;  // can't compare → treat as equal (no update)
    }
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) {
            return va[i] - vb[i];
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Pull OTA — GitHub Release check + download
// ---------------------------------------------------------------------------

#define GITHUB_API_URL "https://api.github.com/repos/grantmd/doorking-esp32/releases/latest"
#define OTA_CHECK_STACK_SIZE  8192
#define OTA_BUF_SIZE          4096

static volatile bool s_ota_in_progress = false;
static esp_timer_handle_t s_check_timer;
static uint32_t s_check_interval_s;
static uint8_t  s_auto_install;

// Lightweight JSON field extractor. Finds "key":"value" and copies value
// into dst. Returns true on success. Works for simple string values only.
static bool json_extract_string(const char *json, const char *key,
                                char *dst, size_t dst_size)
{
    // Build the search pattern: "key":"
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    const char *start = strstr(json, pattern);
    if (!start) {
        return false;
    }
    start += strlen(pattern);

    const char *end = strchr(start, '"');
    if (!end || (size_t)(end - start) >= dst_size) {
        return false;
    }

    size_t len = end - start;
    memcpy(dst, start, len);
    dst[len] = '\0';
    return true;
}

// Find the download URL for doorking-<target>.bin in the release JSON.
// GitHub asset URLs look like:
//   "browser_download_url":"https://github.com/.../doorking-esp32c3.bin"
static bool find_asset_url(const char *json, char *url, size_t url_size)
{
    char asset_name[64];
    snprintf(asset_name, sizeof(asset_name), "doorking-%s.bin", CONFIG_IDF_TARGET);

    // Scan for the asset name, then find the preceding browser_download_url.
    // The assets array lists each asset as an object with "name" and
    // "browser_download_url" fields. We look for our asset name and then
    // extract the URL from the same asset object.
    const char *pos = json;
    while ((pos = strstr(pos, asset_name)) != NULL) {
        // Walk backward to find the start of this asset object (the '{').
        const char *obj_start = pos;
        int depth = 0;
        while (obj_start > json) {
            obj_start--;
            if (*obj_start == '}') depth++;
            if (*obj_start == '{') {
                if (depth == 0) break;
                depth--;
            }
        }

        // Extract browser_download_url from this object.
        const char *url_key = "\"browser_download_url\":\"";
        const char *url_start = strstr(obj_start, url_key);
        if (url_start && url_start < pos) {
            url_start += strlen(url_key);
            const char *url_end = strchr(url_start, '"');
            if (url_end && (size_t)(url_end - url_start) < url_size) {
                size_t len = url_end - url_start;
                memcpy(url, url_start, len);
                url[len] = '\0';
                return true;
            }
        }
        pos++;  // keep scanning in case of false match
    }
    return false;
}

// Download a firmware binary from url and flash it to the next OTA partition.
static esp_err_t download_and_flash(const char *url)
{
    ESP_LOGI(TAG, "downloading OTA image from %s", url);
    status_led_set_state(STATUS_LED_OTA_IN_PROGRESS);

    // GitHub's browser_download_url redirects (302) to their CDN.
    // esp_http_client_open + manual read doesn't follow redirects, so we
    // handle them manually: open, check for 3xx, read Location, re-open.
    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = OTA_BUF_SIZE,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "http_client_init failed");
        status_led_set_state(STATUS_LED_WIFI_CONNECTED);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/octet-stream");

    // Follow redirects manually (up to 5 hops).
    esp_err_t err;
    for (int redirects = 0; redirects < 5; redirects++) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            status_led_set_state(STATUS_LED_WIFI_CONNECTED);
            return err;
        }

        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 200) {
            break;  // we're at the final URL
        }

        if (status >= 300 && status < 400) {
            // The client internally captures the Location header.
            // esp_http_client_set_redirection reads it and updates the URL.
            esp_http_client_close(client);
            err = esp_http_client_set_redirection(client);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "redirect %d but set_redirection failed: %s",
                         status, esp_err_to_name(err));
                esp_http_client_cleanup(client);
                status_led_set_state(STATUS_LED_WIFI_CONNECTED);
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "following redirect %d", status);
            // CDN doesn't need the Accept header.
            esp_http_client_delete_header(client, "Accept");
            continue;
        }

        ESP_LOGE(TAG, "unexpected HTTP status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        status_led_set_state(STATUS_LED_WIFI_CONNECTED);
        return ESP_FAIL;
    }

    int content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "HTTP 200, content-length=%d", content_length);

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        ESP_LOGE(TAG, "no OTA partition available");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        status_led_set_state(STATUS_LED_WIFI_CONNECTED);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        status_led_set_state(STATUS_LED_WIFI_CONNECTED);
        return err;
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "malloc failed for OTA buffer");
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        status_led_set_state(STATUS_LED_WIFI_CONNECTED);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    while (1) {
        int read_len = esp_http_client_read(client, buf, OTA_BUF_SIZE);
        if (read_len < 0) {
            ESP_LOGE(TAG, "http read error");
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            // Check if we've finished reading or if the connection was closed early.
            if (esp_http_client_is_complete_data_received(client)) {
                err = ESP_OK;
            } else {
                ESP_LOGE(TAG, "connection closed before all data received");
                err = ESP_FAIL;
            }
            break;
        }

        err = esp_ota_write(ota_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            break;
        }
        total += read_len;
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        status_led_set_state(STATUS_LED_WIFI_CONNECTED);
        return err;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s (image invalid?)", esp_err_to_name(err));
        status_led_set_state(STATUS_LED_WIFI_CONNECTED);
        return err;
    }

    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        status_led_set_state(STATUS_LED_WIFI_CONNECTED);
        return err;
    }

    ESP_LOGI(TAG, "pull OTA complete (%d bytes), rebooting in 1 s", total);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;  // unreachable
}

// Task that checks GitHub for a new release and optionally installs it.
static void ota_check_task(void *arg)
{
    bool install = (bool)(uintptr_t)arg;

    ESP_LOGI(TAG, "checking GitHub for updates...");

    esp_http_client_config_t cfg = {
        .url = GITHUB_API_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = OTA_BUF_SIZE,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http_client_init failed for check");
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    // GitHub API requires a User-Agent header.
    esp_http_client_set_header(client, "User-Agent", "doorking-esp32");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status != 200) {
        ESP_LOGW(TAG, "GitHub API returned %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    // Read the JSON response. GitHub's latest-release response is typically
    // 2-8 KB depending on release notes. Allocate enough for a reasonable
    // response; truncate if larger (we only need tag_name and asset URLs).
    int json_buf_size = (content_length > 0 && content_length < 16384)
                        ? content_length + 1 : 16384;
    char *json = malloc(json_buf_size);
    if (!json) {
        ESP_LOGE(TAG, "malloc failed for JSON (%d bytes)", json_buf_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    int total = 0;
    while (total < json_buf_size - 1) {
        int read_len = esp_http_client_read(client, json + total,
                                            json_buf_size - 1 - total);
        if (read_len <= 0) break;
        total += read_len;
    }
    json[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // Extract tag_name and asset URL.
    char tag[32] = {0};
    char url[512] = {0};

    if (!json_extract_string(json, "tag_name", tag, sizeof(tag))) {
        ESP_LOGW(TAG, "could not find tag_name in release JSON");
        free(json);
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    const char *current = esp_app_get_description()->version;
    ESP_LOGI(TAG, "latest release: %s, running: %s", tag, current);

    if (semver_cmp(tag, current) <= 0) {
        ESP_LOGI(TAG, "already up to date");
        s_available_version[0] = '\0';
        free(json);
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    if (!find_asset_url(json, url, sizeof(url))) {
        ESP_LOGW(TAG, "no asset matching doorking-%s.bin in release %s",
                 CONFIG_IDF_TARGET, tag);
        free(json);
        s_ota_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    free(json);

    // Cache the available version and URL for /health and the dashboard.
    // Strip leading 'v' for the cached version string.
    const char *ver = tag;
    if (*ver == 'v' || *ver == 'V') ver++;
    strncpy(s_available_version, ver, sizeof(s_available_version) - 1);
    s_available_version[sizeof(s_available_version) - 1] = '\0';
    strncpy(s_asset_url, url, sizeof(s_asset_url) - 1);
    s_asset_url[sizeof(s_asset_url) - 1] = '\0';

    ESP_LOGI(TAG, "update available: %s -> %s", current, s_available_version);

    if (install) {
        download_and_flash(s_asset_url);
        // download_and_flash reboots on success; if we get here it failed.
        ESP_LOGE(TAG, "pull OTA install failed");
    }

    s_ota_in_progress = false;
    vTaskDelete(NULL);
}

esp_err_t ota_check_now(void)
{
    if (s_ota_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ota_in_progress = true;
    BaseType_t ret = xTaskCreate(ota_check_task, "ota_check",
                                 OTA_CHECK_STACK_SIZE, (void *)(uintptr_t)false,
                                 5, NULL);
    if (ret != pdPASS) {
        s_ota_in_progress = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_pull_now(void)
{
    if (s_ota_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ota_in_progress = true;
    BaseType_t ret = xTaskCreate(ota_check_task, "ota_pull",
                                 OTA_CHECK_STACK_SIZE, (void *)(uintptr_t)true,
                                 5, NULL);
    if (ret != pdPASS) {
        s_ota_in_progress = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// Schedule the next periodic check.
static void schedule_next_check(void)
{
    if (s_check_timer && s_check_interval_s > 0) {
        esp_timer_start_once(s_check_timer, (uint64_t)s_check_interval_s * 1000 * 1000);
    }
}

// Periodic check timer callback — spawns a one-shot check task.
static void check_timer_cb(void *arg)
{
    (void)arg;
    if (s_ota_in_progress) {
        // Busy — reschedule for later.
        schedule_next_check();
        return;
    }
    s_ota_in_progress = true;
    BaseType_t ret = xTaskCreate(ota_check_task, "ota_check",
                                 OTA_CHECK_STACK_SIZE,
                                 (void *)(uintptr_t)s_auto_install,
                                 3, NULL);  // lower priority than pull_now
    if (ret != pdPASS) {
        s_ota_in_progress = false;
        ESP_LOGE(TAG, "failed to create OTA check task");
    }
    // Schedule the next check. If the task installs an update and reboots,
    // this timer never fires — which is fine.
    schedule_next_check();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void ota_init(void)
{
    rollback_init();

    // Read OTA config. We access it once at init rather than keeping a
    // pointer to the full config struct.
    doorking_config_t cfg;
    config_load(&cfg);
    s_check_interval_s = cfg.ota_check_interval_s;
    s_auto_install = cfg.ota_auto_install;

    if (cfg.ota_auto_check) {
        // Start the periodic check timer.
        const esp_timer_create_args_t timer_args = {
            .callback = check_timer_cb,
            .name = "ota_check",
        };
        esp_timer_create(&timer_args, &s_check_timer);

        // First check 30 s after boot (let WiFi settle), then every interval.
        esp_timer_start_once(s_check_timer, 30ULL * 1000 * 1000);
        ESP_LOGI(TAG, "auto-check enabled, first check in 30 s, then every %us",
                 (unsigned)s_check_interval_s);
    }

    ESP_LOGI(TAG, "OTA subsystem initialised (version %s, target %s)",
             esp_app_get_description()->version, CONFIG_IDF_TARGET);
}
