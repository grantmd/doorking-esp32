#include "http_api.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "config.h"
#include "gate_sm.h"
#include "log_buffer.h"

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
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
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
    http_cfg.stack_size  = 8192;
    http_cfg.max_uri_handlers = 8;  // default is 8, explicit for clarity

    esp_err_t err = httpd_start(&s_httpd, &http_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return;
    }

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

    ESP_LOGI(TAG, "http api listening on port %d (/health /logs /status /open /close)",
             http_cfg.server_port);
}
