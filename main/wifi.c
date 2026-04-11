#include "wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

static const char *TAG = "wifi";

static volatile bool  s_sta_got_ip = false;
static httpd_handle_t s_provision_httpd = NULL;

// ---------------------------------------------------------------------------
// Event handler
// ---------------------------------------------------------------------------

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "sta start, connecting");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            ESP_LOGW(TAG, "sta disconnected reason=%d, reconnecting", d->reason);
            s_sta_got_ip = false;
            esp_wifi_connect();
            break;
        }

        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "ap started");
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *d = data;
            ESP_LOGI(TAG, "ap client connected " MACSTR, MAC2STR(d->mac));
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *d = data;
            ESP_LOGI(TAG, "ap client disconnected " MACSTR, MAC2STR(d->mac));
            break;
        }

        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *d = data;
            ESP_LOGI(TAG, "sta got ip " IPSTR, IP2STR(&d->ip_info.ip));
            s_sta_got_ip = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Provisioning HTTP server (AP mode only)
// ---------------------------------------------------------------------------

// Keep the HTML tiny — it ships in the binary. Apple HIG-ish styling so it
// looks reasonable on an iPhone, which is the expected provisioning device.
static const char PROVISION_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>DoorKing setup</title>\n"
    "<style>\n"
    "body{font-family:-apple-system,system-ui,sans-serif;max-width:420px;"
    "margin:2em auto;padding:0 1em;color:#222}\n"
    "h1{font-size:1.4em}\n"
    "p{color:#666;font-size:.9em}\n"
    "label{display:block;margin-top:1em;font-weight:500}\n"
    "input{width:100%;padding:.6em;font-size:1em;box-sizing:border-box;"
    "border:1px solid #ccc;border-radius:6px}\n"
    "button{margin-top:1.5em;width:100%;padding:.8em;font-size:1em;"
    "background:#2d7ff9;color:#fff;border:0;border-radius:6px}\n"
    "</style>\n"
    "</head><body>\n"
    "<h1>DoorKing setup</h1>\n"
    "<p>Enter your home WiFi credentials. The device will reboot and join "
    "your network, then show up to Homebridge on its bearer-authed HTTP API.</p>\n"
    "<form method=\"POST\" action=\"/provision\">\n"
    "<label>SSID<input name=\"ssid\" required autocomplete=\"off\"></label>\n"
    "<label>Password<input name=\"psk\" type=\"password\" required></label>\n"
    "<button type=\"submit\">Save &amp; reboot</button>\n"
    "</form>\n"
    "</body></html>\n";

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PROVISION_HTML, HTTPD_RESP_USE_STRLEN);
}

// URL-decode src in place. Handles '+' -> ' ' and %XX hex escapes.
static void url_decode_inplace(char *src)
{
    char *r = src, *w = src;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// Extract field 'name' from application/x-www-form-urlencoded body. Writes
// the URL-decoded value into out (truncating to out_size-1). Returns true
// if the field was found, false otherwise.
static bool form_get(const char *body, const char *name,
                     char *out, size_t out_size)
{
    size_t name_len = strlen(name);
    const char *p = body;
    while (*p) {
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            p += name_len + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < out_size) {
                out[i++] = *p++;
            }
            out[i] = '\0';
            url_decode_inplace(out);
            return true;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return false;
}

// Fill dst with 64 hex chars of cryptographically-random data plus a NUL.
// dst must be at least 65 bytes.
static void generate_auth_token(char *dst)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t buf[32];
    esp_fill_random(buf, sizeof(buf));
    for (int i = 0; i < 32; i++) {
        dst[i * 2]     = hex[(buf[i] >> 4) & 0xf];
        dst[i * 2 + 1] = hex[buf[i]        & 0xf];
    }
    dst[64] = '\0';
}

// Giving the httpd response time to flush before we reboot. esp_restart
// from inside the handler would cut off the response mid-send.
static void reboot_task(void *arg)
{
    (void)arg;
    // 3 seconds: long enough for the httpd response to flush to the phone
    // over WiFi and for the user to read the token on the success page
    // before the AP disappears. Bumped from 2 s after an iOS captive-
    // portal view failed to render the response cleanly in practice.
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Cleanly close any lingering httpd sockets before we yank WiFi out
    // from under them. Without this, browsers that keep the connection
    // open for a favicon probe or keepalive cause "httpd_sock_err: error
    // in recv : 113 (EHOSTUNREACH)" warnings during shutdown as their
    // sockets hit the unreachable route mid-teardown. Harmless but noisy.
    if (s_provision_httpd) {
        httpd_stop(s_provision_httpd);
        s_provision_httpd = NULL;
    }

    ESP_LOGI(TAG, "rebooting after provisioning");
    esp_restart();
}

static esp_err_t handle_provision(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad content length");
        return ESP_FAIL;
    }
    char *body = malloc((size_t)total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, body + received, total - received);
        if (n <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += n;
    }
    body[total] = '\0';

    doorking_config_t cfg;
    config_load(&cfg);

    if (!form_get(body, "ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid)) ||
        !form_get(body, "psk",  cfg.wifi_psk,  sizeof(cfg.wifi_psk))) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid or psk");
        return ESP_FAIL;
    }
    free(body);

    // Mint a fresh bearer token every time we (re)provision. This is the
    // one and only time it will be shown; users must copy it to Homebridge
    // now.
    generate_auth_token(cfg.auth_token);

    esp_err_t err = config_save(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
        return ESP_FAIL;
    }

    // Also log the token prominently to serial as a backup delivery path.
    // iOS captive-portal views have a habit of failing to render the
    // success response cleanly when the underlying AP is about to go
    // away, so the HTTP page alone is not reliable enough to deliver a
    // one-shot secret.
    ESP_LOGW(TAG, ">>> new bearer token (copy to Homebridge): %s", cfg.auth_token);

    char page[1024];
    snprintf(page, sizeof(page),
        "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Saved</title><style>"
        "body{font-family:-apple-system,system-ui,sans-serif;max-width:420px;"
        "margin:2em auto;padding:0 1em;color:#222}"
        "h1{font-size:1.4em}"
        "code{word-break:break-all;background:#f4f4f4;padding:.75em;"
        "display:block;border-radius:6px;font-size:.9em;margin-top:.5em}"
        "</style></head><body>"
        "<h1>Saved</h1>"
        "<p>Joining <strong>%s</strong> and rebooting in 2 seconds.</p>"
        "<p><strong>Copy this bearer token now</strong> — it is not shown "
        "again without re-provisioning:</p>"
        "<code>%s</code>"
        "</body></html>",
        cfg.wifi_ssid, cfg.auth_token);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void start_provisioning_httpd(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port  = 80;
    cfg.stack_size   = 8192;

    esp_err_t err = httpd_start(&s_provision_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return;
    }

    const httpd_uri_t root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = handle_root,
    };
    httpd_register_uri_handler(s_provision_httpd, &root);

    const httpd_uri_t provision = {
        .uri      = "/provision",
        .method   = HTTP_POST,
        .handler  = handle_provision,
    };
    httpd_register_uri_handler(s_provision_httpd, &provision);

    ESP_LOGI(TAG, "provisioning httpd listening on port 80");
}

// ---------------------------------------------------------------------------
// Mode setup
// ---------------------------------------------------------------------------

static void start_sta(const doorking_config_t *cfg)
{
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid,     cfg->wifi_ssid, sizeof(wc.sta.ssid)     - 1);
    strncpy((char *)wc.sta.password, cfg->wifi_psk,  sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Disable WiFi modem sleep — this is a wall-powered gate controller and
    // we want the lowest-latency response to HTTP commands.
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "sta mode starting, ssid=%s", cfg->wifi_ssid);
}

static void start_ap(void)
{
    wifi_config_t wc = { 0 };
    const char *ap_ssid = "doorking-setup";
    strncpy((char *)wc.ap.ssid, ap_ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len       = strlen(ap_ssid);
    wc.ap.channel        = 1;
    wc.ap.authmode       = WIFI_AUTH_OPEN;
    wc.ap.max_connection = 4;
    wc.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "ap mode starting, ssid=%s (open)", ap_ssid);

    start_provisioning_httpd();
}

void wifi_start(const doorking_config_t *cfg)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (config_has_wifi(cfg)) {
        esp_netif_create_default_wifi_sta();
    } else {
        esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    // Don't let the WiFi driver write its own config to NVS — we manage
    // credentials via the config.c layer ourselves and we don't want two
    // sources of truth fighting over the same NVS pages.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    if (config_has_wifi(cfg)) {
        start_sta(cfg);
    } else {
        start_ap();
    }
}

bool wifi_sta_got_ip(void)
{
    return s_sta_got_ip;
}
