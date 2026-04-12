#include "http_api.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "config.h"
#include "gate_sm.h"
#include "log_buffer.h"
#include "ota.h"

static const char *TAG = "http_api";

static httpd_handle_t s_httpd = NULL;

// Stash the bearer token at server start so handlers don't need to
// re-read NVS on every request.
static char s_auth_token[DOORKING_AUTH_TOKEN_MAX_LEN + 1];

// Pointer to the gate state machine, set by http_api_start. Accessed
// from the httpd task on /status, /open, /close requests. See the
// concurrency note in http_api.h.
static gate_sm_t *s_gate_sm = NULL;

// ---------------------------------------------------------------------------
// Bearer-token auth helper
// ---------------------------------------------------------------------------

// Check the Authorization header for "Bearer <token>". Returns true if
// the token matches s_auth_token, false otherwise. On false the handler
// should return a 401.
static bool check_bearer_auth(httpd_req_t *req)
{
    if (s_auth_token[0] == '\0') {
        // No token configured — reject everything. The user needs to
        // provision first.
        return false;
    }

    // "Bearer " is 7 chars + up to 64 chars of token + NUL.
    char hdr[7 + DOORKING_AUTH_TOKEN_MAX_LEN + 1];
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr));
    if (err != ESP_OK) {
        return false;
    }

    // Expect exactly "Bearer <token>".
    if (strncmp(hdr, "Bearer ", 7) != 0) {
        return false;
    }

    return strcmp(hdr + 7, s_auth_token) == 0;
}

// Convenience: check auth and send 401 if it fails. Returns true if
// the handler should continue, false if it already sent the error
// response and should return ESP_OK immediately.
static bool require_auth(httpd_req_t *req)
{
    if (check_bearer_auth(req)) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"missing or invalid bearer token\"}");
    return false;
}

// ---------------------------------------------------------------------------
// GET /health — unauthenticated liveness probe
// ---------------------------------------------------------------------------

static esp_err_t handle_health(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const char *update_ver = ota_get_available_version();

    char body[256];
    if (update_ver) {
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"version\":\"%s\",\"target\":\"%s\","
                 "\"update_available\":\"%s\"}",
                 app->version, CONFIG_IDF_TARGET, update_ver);
    } else {
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"version\":\"%s\",\"target\":\"%s\","
                 "\"update_available\":null}",
                 app->version, CONFIG_IDF_TARGET);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// ---------------------------------------------------------------------------
// GET /logs — authenticated log buffer dump
// ---------------------------------------------------------------------------

static esp_err_t handle_logs(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }

    size_t len = 0;
    char *snap = log_buffer_snapshot(&len);
    if (!snap || len == 0) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "(log buffer empty)\n");
        free(snap);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, snap, len);
    free(snap);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /status — authenticated gate state
// ---------------------------------------------------------------------------

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }

    gate_state_t    state    = gate_sm_state(s_gate_sm);
    gate_last_cmd_t last_cmd = gate_sm_last_cmd(s_gate_sm);
    uint64_t        cmd_ms   = gate_sm_last_cmd_ms(s_gate_sm);

    uint32_t age_s = 0;
    if (last_cmd != GATE_LAST_CMD_NONE && cmd_ms > 0) {
        uint64_t elapsed = now_ms() - cmd_ms;
        age_s = (uint32_t)(elapsed / 1000);
    }

    char body[192];
    snprintf(body, sizeof(body),
             "{\"state\":\"%s\",\"last_cmd\":\"%s\",\"last_cmd_age_s\":%u}",
             gate_sm_state_name(state),
             gate_sm_last_cmd_name(last_cmd),
             (unsigned)age_s);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// ---------------------------------------------------------------------------
// POST /open — command the gate to open
// ---------------------------------------------------------------------------

static esp_err_t handle_open(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }

    gate_cmd_result_t result = gate_sm_on_cmd_open(s_gate_sm, now_ms());
    httpd_resp_set_type(req, "application/json");

    switch (result) {
    case GATE_CMD_RESULT_ACCEPTED:
        // TODO: pulse the OPEN relay here once relay_i2c.c lands.
        // For now the state machine transitions but no physical relay
        // fires — testable end-to-end via /status without hardware.
        ESP_LOGI(TAG, "POST /open: accepted (relay pulse TODO)");
        return httpd_resp_sendstr(req, "{\"result\":\"accepted\"}");

    case GATE_CMD_RESULT_IDEMPOTENT:
        ESP_LOGI(TAG, "POST /open: idempotent (already open/opening)");
        return httpd_resp_sendstr(req, "{\"result\":\"idempotent\"}");

    case GATE_CMD_RESULT_THROTTLED:
        ESP_LOGW(TAG, "POST /open: throttled");
        httpd_resp_set_status(req, "429 Too Many Requests");
        return httpd_resp_sendstr(req, "{\"error\":\"throttled\"}");
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// POST /close — command the gate to close
// ---------------------------------------------------------------------------

static esp_err_t handle_close(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }

    gate_cmd_result_t result = gate_sm_on_cmd_close(s_gate_sm, now_ms());
    httpd_resp_set_type(req, "application/json");

    switch (result) {
    case GATE_CMD_RESULT_ACCEPTED:
        // TODO: pulse the CLOSE relay here once relay_i2c.c lands.
        ESP_LOGI(TAG, "POST /close: accepted (relay pulse TODO)");
        return httpd_resp_sendstr(req, "{\"result\":\"accepted\"}");

    case GATE_CMD_RESULT_IDEMPOTENT:
        ESP_LOGI(TAG, "POST /close: idempotent (already closed/closing)");
        return httpd_resp_sendstr(req, "{\"result\":\"idempotent\"}");

    case GATE_CMD_RESULT_THROTTLED:
        ESP_LOGW(TAG, "POST /close: throttled");
        httpd_resp_set_status(req, "429 Too Many Requests");
        return httpd_resp_sendstr(req, "{\"error\":\"throttled\"}");
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET / — dashboard page (unauthenticated)
// ---------------------------------------------------------------------------

static esp_err_t handle_dashboard(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const char *update_ver = ota_get_available_version();

    // Build the update section dynamically.
    char update_html[1024];
    if (update_ver) {
        snprintf(update_html, sizeof(update_html),
            "<p><strong>Update available: %s</strong></p>"
            "<button id=\"btn\" onclick=\"doUpdate()\">Update firmware</button>"
            "<p id=\"msg\" style=\"display:none;color:#666;font-size:.9em\"></p>"
            "<script>"
            "function doUpdate(){"
            "var t=prompt('Enter bearer token to authorise the update:');"
            "if(!t)return;"
            "var b=document.getElementById('btn'),m=document.getElementById('msg');"
            "b.disabled=true;b.textContent='Updating\\u2026';"
            "m.style.display='block';m.textContent='Downloading and flashing. Do not power off.';"
            "fetch('/update/pull',{method:'POST',headers:{'Authorization':'Bearer '+t}})"
            ".then(function(r){return r.json()})"
            ".then(function(d){"
            "if(d.rebooting){m.textContent='Update installed. Rebooting\\u2026';}"
            "else{m.textContent='Error: '+(d.error||'unknown');b.disabled=false;b.textContent='Update firmware';}"
            "})"
            ".catch(function(e){m.textContent='Network error: '+e;b.disabled=false;b.textContent='Update firmware';});"
            "}"
            "</script>",
            update_ver);
    } else {
        snprintf(update_html, sizeof(update_html),
            "<p style=\"color:#666\">Up to date</p>");
    }

    char body[2560];
    snprintf(body, sizeof(body),
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
        "<title>doorking-esp32</title>\n"
        "<style>\n"
        "body{font-family:-apple-system,system-ui,sans-serif;max-width:420px;"
        "margin:2em auto;padding:0 1em;color:#222}\n"
        "h1{font-size:1.4em}\n"
        "a{color:#2d7ff9}\n"
        "table{border-collapse:collapse;width:100%%}\n"
        "td{padding:.3em 0}\n"
        "td:first-child{color:#666;padding-right:1em}\n"
        "button{margin-top:.5em;padding:.6em 1.2em;font-size:1em;"
        "background:#2d7ff9;color:#fff;border:0;border-radius:6px;cursor:pointer}\n"
        "button:disabled{background:#999;cursor:wait}\n"
        "</style>\n"
        "</head><body>\n"
        "<h1>doorking-esp32</h1>\n"
        "<p><a href=\"https://github.com/grantmd/doorking-esp32\">github.com/grantmd/doorking-esp32</a></p>\n"
        "<table>\n"
        "<tr><td>Firmware</td><td>%s</td></tr>\n"
        "<tr><td>Target</td><td>%s</td></tr>\n"
        "</table>\n"
        "<hr style=\"margin:1em 0;border:0;border-top:1px solid #ddd\">\n"
        "%s\n"
        "</body></html>\n",
        app->version, CONFIG_IDF_TARGET, update_html);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, body);
}

// ---------------------------------------------------------------------------
// POST /update — push OTA firmware upload
// ---------------------------------------------------------------------------

static esp_err_t handle_push_update(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    return ota_push_from_http(req);
}

// ---------------------------------------------------------------------------
// POST /update/pull — trigger pull OTA from GitHub
// ---------------------------------------------------------------------------

static esp_err_t handle_pull_update(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }

    esp_err_t err = ota_pull_now();
    httpd_resp_set_type(req, "application/json");

    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"OTA already in progress\"}");
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"failed to start OTA\"}");
    }

    return httpd_resp_sendstr(req, "{\"result\":\"ok\",\"rebooting\":true}");
}

// ---------------------------------------------------------------------------
// Server lifecycle
// ---------------------------------------------------------------------------

void http_api_start(const doorking_config_t *cfg, gate_sm_t *sm)
{
    if (s_httpd) {
        return;  // already running
    }

    // Stash the bearer token for the lifetime of the server.
    strncpy(s_auth_token, cfg->auth_token, sizeof(s_auth_token) - 1);
    s_auth_token[sizeof(s_auth_token) - 1] = '\0';

    // Stash the gate state machine pointer for /status, /open, /close.
    s_gate_sm = sm;

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port = 80;
    http_cfg.stack_size  = 10240;  // bumped from 8192 for OTA write path
    http_cfg.max_uri_handlers = 10;  // / + /health + /logs + /status + /open + /close + /update + /update/pull

    esp_err_t err = httpd_start(&s_httpd, &http_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return;
    }

    const httpd_uri_t dashboard = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = handle_dashboard,
    };
    httpd_register_uri_handler(s_httpd, &dashboard);

    const httpd_uri_t health = {
        .uri     = "/health",
        .method  = HTTP_GET,
        .handler = handle_health,
    };
    httpd_register_uri_handler(s_httpd, &health);

    const httpd_uri_t logs = {
        .uri     = "/logs",
        .method  = HTTP_GET,
        .handler = handle_logs,
    };
    httpd_register_uri_handler(s_httpd, &logs);

    const httpd_uri_t status = {
        .uri     = "/status",
        .method  = HTTP_GET,
        .handler = handle_status,
    };
    httpd_register_uri_handler(s_httpd, &status);

    const httpd_uri_t open = {
        .uri     = "/open",
        .method  = HTTP_POST,
        .handler = handle_open,
    };
    httpd_register_uri_handler(s_httpd, &open);

    const httpd_uri_t close = {
        .uri     = "/close",
        .method  = HTTP_POST,
        .handler = handle_close,
    };
    httpd_register_uri_handler(s_httpd, &close);

    const httpd_uri_t update = {
        .uri     = "/update",
        .method  = HTTP_POST,
        .handler = handle_push_update,
    };
    httpd_register_uri_handler(s_httpd, &update);

    const httpd_uri_t pull = {
        .uri     = "/update/pull",
        .method  = HTTP_POST,
        .handler = handle_pull_update,
    };
    httpd_register_uri_handler(s_httpd, &pull);

    ESP_LOGI(TAG, "http api listening on port %d (/ /health /logs /status /open /close /update /update/pull)",
             http_cfg.server_port);
}
