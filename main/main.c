// doorking — ESP-IDF firmware bridging a DoorKing 4602-010 slide-gate
// controller to HomeKit via Homebridge over a bearer-authenticated HTTP API.
//
// Multi-target: the same firmware runs on any supported reference board
// (Seeed XIAO ESP32-C3, SparkFun Thing Plus ESP32 WROOM, SparkFun Thing
// Plus ESP32-C5) by selecting the ESP-IDF target. Board-specific pin
// assignments and the board name live in main/board.h; target-specific
// Kconfig defaults live in sdkconfig.defaults.<target>.
//
// This file boots the modules in order: log buffer (first, so boot
// output is captured) → NVS → config → WiFi (STA or AP provisioning)
// → reset-button watcher → HTTP API → gate state machine tick loop.

#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "board.h"
#include "config.h"
#include "gate_sm.h"
#include "http_api.h"
#include "i2c_bus.h"
#include "log_buffer.h"
#include "relay_i2c.h"
#include "reset_button.h"
#include "ota.h"
#include "status_input.h"
#include "status_led.h"
#include "wifi.h"

static const char *TAG = "doorking";

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

// Bring up NVS flash. If the partition is new or the layout has changed
// (e.g. after an OTA that added new keys), erase and re-init so boot
// always succeeds. This matches the pattern in every ESP-IDF example.
static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs needs erase (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    // Install the log ring buffer FIRST so every subsequent ESP_LOGx
    // call is captured — including boot-time output from NVS, WiFi,
    // and the provisioning server.
    log_buffer_init();

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "doorking firmware booting");
    ESP_LOGI(TAG, "board: %s", BOARD_NAME);
    ESP_LOGI(TAG, "chip:  %s rev %d.%d, %d core(s)",
             CONFIG_IDF_TARGET,
             chip.revision / 100, chip.revision % 100,
             chip.cores);

    init_nvs();

    doorking_config_t cfg;
    ESP_ERROR_CHECK(config_load(&cfg));
    config_log(&cfg);

    // Init the gate state machine before the HTTP API so we can pass
    // its pointer to the API handlers.
    static gate_sm_t sm;
    const gate_sm_config_t sm_cfg = {
        .travel_timeout_ms  = cfg.travel_timeout_ms,
        .min_cmd_spacing_ms = cfg.min_cmd_spacing_ms,
    };
    gate_sm_init(&sm, &sm_cfg);
    ESP_LOGI(TAG, "gate_sm initial state: %s", gate_sm_state_name(gate_sm_state(&sm)));

    status_led_init();
    wifi_start(&cfg);
    reset_button_start();
    status_input_start(&sm);

    // Bring up the I²C bus and scan for devices. The bus handle is
    // shared: relay_i2c registers OPEN/CLOSE device handles on it,
    // and http_api uses it for the /i2c/scan + /i2c/relay-address
    // diagnostic endpoints.
    i2c_master_bus_handle_t i2c_bus = i2c_bus_init_and_scan();

    const relay_i2c_config_t relay_cfg = {
        .bus        = i2c_bus,
        .open_addr  = cfg.relay_open_addr,
        .close_addr = cfg.relay_close_addr,
        .pulse_ms   = cfg.pulse_ms,
    };
    // A missing relay at boot is logged but not fatal; see relay_i2c.h.
    (void)relay_i2c_init(&relay_cfg);

    // Start the main HTTP API and mDNS. Only useful in STA mode (the
    // provisioning server in wifi.c handles AP mode).
    if (config_has_wifi(&cfg)) {
        http_api_start(&cfg, &sm, i2c_bus);

        // Advertise as <hostname>.local on the LAN so the user (and
        // Homebridge) can reach the device without knowing its DHCP IP.
        // Suppress transient mDNS allocation errors that happen when TLS
        // handshakes temporarily starve the heap. These are harmless —
        // mDNS recovers when the TLS connection closes — but they fill
        // the 16 KB log ring buffer with noise.
        esp_log_level_set("mdns_send", ESP_LOG_NONE);
        esp_log_level_set("mdns_receive", ESP_LOG_NONE);
        esp_log_level_set("mdns_networking", ESP_LOG_NONE);
        ESP_ERROR_CHECK(mdns_init());
        ESP_ERROR_CHECK(mdns_hostname_set(cfg.hostname));
        mdns_instance_name_set("DoorKing Gate Bridge");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mdns: %s.local", cfg.hostname);

        ota_init();
    }

    while (1) {
        gate_sm_on_tick(&sm, now_ms());
        // Heartbeat is debug-level: keeps the tick loop visible during
        // dev but doesn't fill the 16 KB log ring buffer with noise in
        // production. Use `idf.py monitor` or set the runtime log level
        // to DEBUG to see these.
        ESP_LOGD(TAG, "tick state=%s", gate_sm_state_name(gate_sm_state(&sm)));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
