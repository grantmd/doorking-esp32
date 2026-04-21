#include "http_api.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "gate_sm.h"
#include "i2c_bus.h"
#include "log_buffer.h"
#include "ota.h"
#include "relay_i2c.h"

static const char *TAG = "http_api";

static httpd_handle_t s_httpd = NULL;

// Stash the bearer token at server start so handlers don't need to
// re-read NVS on every request.
static char s_auth_token[DOORKING_AUTH_TOKEN_MAX_LEN + 1];

// Pointer to the gate state machine, set by http_api_start. Accessed
// from the httpd task on /status, /open, /close requests. See the
// concurrency note in http_api.h.
static gate_sm_t *s_gate_sm = NULL;

// I²C bus handle, set by http_api_start. Used by the /i2c/* diagnostic
// and one-shot device-config endpoints. NULL if the caller didn't
// bring one up (e.g. a future host-side test harness).
static i2c_master_bus_handle_t s_i2c_bus = NULL;

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
    case GATE_CMD_RESULT_ACCEPTED: {
        ESP_LOGI(TAG, "POST /open: accepted, pulsing OPEN relay");
        esp_err_t err = relay_i2c_pulse_open();
        if (err != ESP_OK) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            char body[128];
            snprintf(body, sizeof(body),
                     "{\"error\":\"relay pulse failed\",\"code\":\"%s\"}",
                     esp_err_to_name(err));
            return httpd_resp_sendstr(req, body);
        }
        return httpd_resp_sendstr(req, "{\"result\":\"accepted\"}");
    }

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
    case GATE_CMD_RESULT_ACCEPTED: {
        ESP_LOGI(TAG, "POST /close: accepted, pulsing CLOSE relay");
        esp_err_t err = relay_i2c_pulse_close();
        if (err != ESP_OK) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            char body[128];
            snprintf(body, sizeof(body),
                     "{\"error\":\"relay pulse failed\",\"code\":\"%s\"}",
                     esp_err_to_name(err));
            return httpd_resp_sendstr(req, body);
        }
        return httpd_resp_sendstr(req, "{\"result\":\"accepted\"}");
    }

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
    char update_html[256];
    if (update_ver) {
        snprintf(update_html, sizeof(update_html),
            "<p><strong>Update available: %s</strong></p>"
            "<button class=\"btn\" onclick=\"doUpdate()\">Update firmware</button>",
            update_ver);
    } else {
        snprintf(update_html, sizeof(update_html),
            "<p style=\"color:#666\">Up to date</p>");
    }

    // The page is small enough to build in a single snprintf. The shared
    // JavaScript handles auth-prompted POST actions for all three buttons.
    char body[4096];
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
        ".btn{margin-top:.5em;margin-right:.5em;padding:.6em 1.2em;font-size:1em;"
        "background:#2d7ff9;color:#fff;border:0;border-radius:6px;cursor:pointer}\n"
        ".btn:disabled{background:#999;cursor:wait}\n"
        ".btn-secondary{background:#666}\n"
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
        "<hr style=\"margin:1em 0;border:0;border-top:1px solid #ddd\">\n"
        "<button class=\"btn btn-secondary\" onclick=\"doCheck()\">Check for updates</button>"
        "<button class=\"btn btn-secondary\" onclick=\"doReboot()\">Reboot</button>"
        "<p id=\"msg\" style=\"display:none;color:#666;font-size:.9em\"></p>\n"
        "<script>\n"
        "var tk=null;\n"
        "function getToken(){"
        "if(!tk){tk=prompt('Enter bearer token:');}"
        "return tk;}\n"
        // Poll /update/status until the operation completes.
        //   expectReboot=true  → a successful outcome will crash the poll
        //                        connection (device reboots); treat that
        //                        as success.
        //   expectReboot=false → a successful outcome returns to 'idle';
        //                        reload soon to pick up the new banner.
        "function pollStatus(expectReboot){\n"
        "var m=document.getElementById('msg');\n"
        "var iv=setInterval(function(){\n"
        "fetch('/update/status')\n"
        ".then(function(r){return r.json();})\n"
        ".then(function(d){\n"
        "if(d.state==='failed'){"
        "clearInterval(iv);"
        "m.textContent='Update failed: '+(d.error||'unknown error');"
        "return;}\n"
        "if(d.state==='idle'){"
        "clearInterval(iv);"
        "m.textContent='Done. Reloading\\u2026';"
        "setTimeout(function(){location.reload();},1500);"
        "return;}\n"
        "if(d.state==='checking'){m.textContent='Checking for updates\\u2026';}\n"
        "if(d.state==='downloading'){m.textContent='Downloading and flashing. Do not power off.';}\n"
        "})\n"
        ".catch(function(){"
        // Connection lost. If we expected a reboot, that's success; wait
        // for the device to come back. If not, surface the network error.
        "clearInterval(iv);"
        "if(expectReboot){"
        "m.textContent='Device rebooting with new firmware\\u2026';"
        "setTimeout(function(){location.reload();},15000);"
        "}else{"
        "m.textContent='Lost connection to device.';"
        "}"
        "});\n"
        "},2000);\n"
        "}\n"
        "function doCheck(){\n"
        "var m=document.getElementById('msg');\n"
        "m.style.display='block';m.textContent='Checking for updates\\u2026';\n"
        "fetch('/update/check',{method:'POST'})\n"
        ".then(function(r){return r.json();})\n"
        ".then(function(d){\n"
        "if(d.error){m.textContent='Error: '+d.error;return;}\n"
        "pollStatus(false);\n"
        "})\n"
        ".catch(function(e){m.textContent='Network error: '+e;});\n"
        "}\n"
        "function doUpdate(){\n"
        "if(!getToken())return;\n"
        "var m=document.getElementById('msg');\n"
        "m.style.display='block';m.textContent='Starting update\\u2026';\n"
        "fetch('/update/pull',{method:'POST',headers:{'Authorization':'Bearer '+tk}})\n"
        ".then(function(r){"
        "if(r.status===401){tk=null;m.textContent='Invalid token.';return null;}\n"
        "return r.json();})\n"
        ".then(function(d){\n"
        "if(!d)return;\n"
        "if(d.error){m.textContent='Error: '+d.error;return;}\n"
        "pollStatus(true);\n"
        "})\n"
        ".catch(function(e){m.textContent='Network error: '+e;});\n"
        "}\n"
        "function doReboot(){\n"
        "if(!getToken())return;\n"
        "var m=document.getElementById('msg');\n"
        "m.style.display='block';m.textContent='Rebooting\\u2026';\n"
        "fetch('/reboot',{method:'POST',headers:{'Authorization':'Bearer '+tk}})\n"
        ".then(function(r){"
        "if(r.status===401){tk=null;m.textContent='Invalid token.';return;}\n"
        "m.textContent='Rebooting. Reloading in 10s\\u2026';"
        "setTimeout(function(){location.reload();},10000);"
        "})\n"
        ".catch(function(e){m.textContent='Network error: '+e;});\n"
        "}\n"
        "</script>\n"
        "</body></html>\n",
        app->version, CONFIG_IDF_TARGET, update_html);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, body);
}

// ---------------------------------------------------------------------------
// GET /favicon.ico — silent 204, suppresses the 404-per-pageview log spam
// ---------------------------------------------------------------------------

static esp_err_t handle_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
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
// GET /update/status — current OTA operation state (no auth)
//
// The dashboard polls this while a check or update is running so the user
// sees progress and errors instead of a page that looks like it's just
// sitting there. Unauthenticated for the same reason as /update/check —
// nothing sensitive here, and the version info is already public.
// ---------------------------------------------------------------------------

static esp_err_t handle_update_status(httpd_req_t *req)
{
    ota_state_t state = ota_get_state();
    const char *err   = ota_get_last_error();

    char body[192];
    if (err[0]) {
        // Escape quotes in the error message conservatively — our internal
        // error strings never contain them, but be defensive.
        char safe_err[96];
        size_t j = 0;
        for (size_t i = 0; err[i] && j + 2 < sizeof(safe_err); i++) {
            if (err[i] == '"' || err[i] == '\\') {
                safe_err[j++] = '\\';
            }
            safe_err[j++] = err[i];
        }
        safe_err[j] = '\0';
        snprintf(body, sizeof(body),
                 "{\"state\":\"%s\",\"error\":\"%s\"}",
                 ota_state_name(state), safe_err);
    } else {
        snprintf(body, sizeof(body),
                 "{\"state\":\"%s\",\"error\":null}",
                 ota_state_name(state));
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// ---------------------------------------------------------------------------
// POST /update/check — trigger an update check against GitHub (no auth)
//
// Left unauthenticated intentionally: the version info this resolves is
// already exposed on GET / and GET /health, and the endpoint's only side
// effect is a single outbound GitHub API call. ota_check_now has a
// check-in-progress guard (ESP_ERR_INVALID_STATE) that limits the impact
// of rapid-fire requests to a 409 reply — they don't pile up upstream.
// ---------------------------------------------------------------------------

static esp_err_t handle_check_update(httpd_req_t *req)
{
    esp_err_t err = ota_check_now();
    httpd_resp_set_type(req, "application/json");

    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"check already in progress\"}");
    }
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"failed to start check\"}");
    }

    return httpd_resp_sendstr(req, "{\"result\":\"ok\"}");
}

// ---------------------------------------------------------------------------
// POST /reboot — authenticated device restart
// ---------------------------------------------------------------------------

static void delayed_reboot(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t handle_reboot(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "POST /reboot: rebooting in 1 s");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"result\":\"ok\",\"rebooting\":true}");
    xTaskCreate(delayed_reboot, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GET /i2c/scan — enumerate ACK'd I²C addresses
// ---------------------------------------------------------------------------

static esp_err_t handle_i2c_scan(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");

    if (!s_i2c_bus) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"i2c bus not initialized\"}");
    }

    // 16 devices is plenty — the 7-bit address space on a gate-bridge
    // board realistically sees <4 (fuel gauge + 2 relays + headroom).
    i2c_bus_scan_entry_t entries[16];
    size_t found = i2c_bus_scan(s_i2c_bus, entries, sizeof(entries) / sizeof(entries[0]));
    size_t reported = (found > sizeof(entries) / sizeof(entries[0]))
                    ? sizeof(entries) / sizeof(entries[0]) : found;

    // 16 entries × ~60 bytes of JSON each + wrapper → 1 KB is plenty.
    char body[1200];
    int n = snprintf(body, sizeof(body), "{\"count\":%u,\"devices\":[", (unsigned)found);
    for (size_t i = 0; i < reported && n < (int)sizeof(body); i++) {
        n += snprintf(body + n, sizeof(body) - n,
                      "%s{\"addr\":\"0x%02x\",\"label\":\"%s\"}",
                      i ? "," : "",
                      entries[i].addr,
                      entries[i].label);
    }
    if (n < (int)sizeof(body)) {
        snprintf(body + n, sizeof(body) - n, "]}");
    }

    return httpd_resp_sendstr(req, body);
}

// ---------------------------------------------------------------------------
// POST /i2c/relay-address — reassign a Qwiic Single Relay's I²C address
//
// Query: ?from=0x18&to=0x19 (hex accepted with or without 0x prefix)
// Sends the SparkFun SINGLE_CHANGE_ADDRESS command (register 0x03,
// payload = new address). The relay persists the new address to
// onboard EEPROM and responds at the new address immediately.
// ---------------------------------------------------------------------------

// Parse a hex or decimal address from a query-string value. Returns
// true on success and writes the parsed byte to *out. Accepts "0x18",
// "18", "24". Rejects anything outside [0x07, 0x78] — the valid I²C
// range the SparkFun firmware enforces.
static bool parse_i2c_addr(const char *s, uint8_t *out)
{
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    long v = strtol(s, &end, 0);  // base 0: honors 0x prefix
    if (end == s || *end != '\0') {
        return false;
    }
    if (v < 0x07 || v > 0x78) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

static esp_err_t handle_i2c_relay_address(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");

    if (!s_i2c_bus) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"i2c bus not initialized\"}");
    }

    // Pull ?from=...&to=... out of the query string.
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"missing ?from=0xNN&to=0xNN\"}");
    }

    char from_s[8] = {0};
    char to_s[8]   = {0};
    if (httpd_query_key_value(query, "from", from_s, sizeof(from_s)) != ESP_OK ||
        httpd_query_key_value(query, "to",   to_s,   sizeof(to_s))   != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"missing ?from=0xNN&to=0xNN\"}");
    }

    uint8_t from_addr = 0, to_addr = 0;
    if (!parse_i2c_addr(from_s, &from_addr) || !parse_i2c_addr(to_s, &to_addr)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"addresses must be in 0x07..0x78\"}");
    }

    // Sanity-check: the source address must currently ACK. If it
    // doesn't, we'd be blasting the command into the void — or worse,
    // into the wrong device — so fail loudly.
    if (i2c_master_probe(s_i2c_bus, from_addr, 50) != ESP_OK) {
        httpd_resp_set_status(req, "404 Not Found");
        char body[96];
        snprintf(body, sizeof(body),
                 "{\"error\":\"no device at 0x%02x\"}", from_addr);
        return httpd_resp_sendstr(req, body);
    }

    esp_err_t err = i2c_bus_relay_change_address(s_i2c_bus, from_addr, to_addr);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char body[128];
        snprintf(body, sizeof(body),
                 "{\"error\":\"change-address transmit failed\",\"code\":\"%s\"}",
                 esp_err_to_name(err));
        return httpd_resp_sendstr(req, body);
    }

    // Verify: the relay should now ACK at the new address and no longer
    // at the old one. The ATtiny's EEPROM write plus the re-init on the
    // new address takes long enough that probing too soon hits a
    // hardware timeout — 50 ms was empirically too short, 250 ms is
    // comfortable without making the endpoint feel sluggish.
    vTaskDelay(pdMS_TO_TICKS(250));
    bool new_ok   = (i2c_master_probe(s_i2c_bus, to_addr,   100) == ESP_OK);
    bool old_gone = (i2c_master_probe(s_i2c_bus, from_addr, 100) != ESP_OK);

    ESP_LOGI(TAG, "relay address reassign 0x%02x -> 0x%02x: new_ok=%d old_gone=%d",
             from_addr, to_addr, new_ok, old_gone);

    char body[160];
    snprintf(body, sizeof(body),
             "{\"ok\":%s,\"from\":\"0x%02x\",\"to\":\"0x%02x\","
             "\"new_ack\":%s,\"old_gone\":%s}",
             (new_ok && old_gone) ? "true" : "false",
             from_addr, to_addr,
             new_ok ? "true" : "false",
             old_gone ? "true" : "false");
    return httpd_resp_sendstr(req, body);
}

// ---------------------------------------------------------------------------
// Server lifecycle
// ---------------------------------------------------------------------------

void http_api_start(const doorking_config_t *cfg,
                    gate_sm_t *sm,
                    i2c_master_bus_handle_t i2c_bus)
{
    if (s_httpd) {
        return;  // already running
    }

    // Stash the bearer token for the lifetime of the server.
    strncpy(s_auth_token, cfg->auth_token, sizeof(s_auth_token) - 1);
    s_auth_token[sizeof(s_auth_token) - 1] = '\0';

    // Stash the gate state machine pointer for /status, /open, /close.
    s_gate_sm = sm;

    // Stash the I²C bus handle for the /i2c/* diagnostic endpoints.
    s_i2c_bus = i2c_bus;

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port = 80;
    http_cfg.stack_size  = 10240;  // bumped from 8192 for OTA write path
    http_cfg.max_uri_handlers = 16;

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

    const httpd_uri_t favicon = {
        .uri     = "/favicon.ico",
        .method  = HTTP_GET,
        .handler = handle_favicon,
    };
    httpd_register_uri_handler(s_httpd, &favicon);

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

    const httpd_uri_t check = {
        .uri     = "/update/check",
        .method  = HTTP_POST,
        .handler = handle_check_update,
    };
    httpd_register_uri_handler(s_httpd, &check);

    const httpd_uri_t update_status = {
        .uri     = "/update/status",
        .method  = HTTP_GET,
        .handler = handle_update_status,
    };
    httpd_register_uri_handler(s_httpd, &update_status);

    const httpd_uri_t reboot = {
        .uri     = "/reboot",
        .method  = HTTP_POST,
        .handler = handle_reboot,
    };
    httpd_register_uri_handler(s_httpd, &reboot);

    const httpd_uri_t i2c_scan = {
        .uri     = "/i2c/scan",
        .method  = HTTP_GET,
        .handler = handle_i2c_scan,
    };
    httpd_register_uri_handler(s_httpd, &i2c_scan);

    const httpd_uri_t i2c_relay_addr = {
        .uri     = "/i2c/relay-address",
        .method  = HTTP_POST,
        .handler = handle_i2c_relay_address,
    };
    httpd_register_uri_handler(s_httpd, &i2c_relay_addr);

    ESP_LOGI(TAG, "http api listening on port %d (/ /health /logs /status /open /close /update /update/check /update/pull /update/status /reboot /i2c/scan /i2c/relay-address)",
             http_cfg.server_port);
}
