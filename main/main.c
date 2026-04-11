// doorking - ESP32-C3 bridge for DoorKing 4602-010 gate controller.
//
// Phase 1 target: prove the toolchain + XIAO ESP32-C3 are wired up correctly by
// booting, logging over USB Serial/JTAG, and heartbeating once per second. Real
// WiFi / HTTP / gate logic lands in later commits once this runs.

#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "gate_sm.h"

static const char *TAG = "doorking";

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "doorking firmware booting");
    ESP_LOGI(TAG, "chip: %s rev %d.%d, %d core(s), flash %dMB",
             CONFIG_IDF_TARGET,
             chip.revision / 100, chip.revision % 100,
             chip.cores,
             CONFIG_ESPTOOLPY_FLASHSIZE_4MB ? 4 : 0);

    // Bring up the gate state machine with defaults. Hardware wiring and
    // real status reads land in a later commit; for now this just proves
    // the module links and initialises on-target.
    gate_sm_t sm;
    const gate_sm_config_t cfg = {
        .travel_timeout_ms  = 30000,
        .min_cmd_spacing_ms = 2000,
    };
    gate_sm_init(&sm, &cfg);
    ESP_LOGI(TAG, "gate_sm initial state: %s", gate_sm_state_name(gate_sm_state(&sm)));

    uint32_t tick = 0;
    while (1) {
        gate_sm_on_tick(&sm, now_ms());
        ESP_LOGI(TAG, "heartbeat %" PRIu32 " state=%s",
                 tick++, gate_sm_state_name(gate_sm_state(&sm)));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
