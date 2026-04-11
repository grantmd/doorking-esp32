// doorking - ESP32-C3 bridge for DoorKing 4602-010 gate controller.
//
// Phase 1 target: prove the toolchain + XIAO ESP32-C3 are wired up correctly by
// booting, logging over USB Serial/JTAG, and heartbeating once per second. Real
// WiFi / HTTP / gate logic lands in later commits once this runs.

#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "doorking";

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

    uint32_t tick = 0;
    while (1) {
        ESP_LOGI(TAG, "heartbeat %" PRIu32, tick++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
