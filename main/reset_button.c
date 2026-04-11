#include "reset_button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "config.h"
#include "reset_btn_sm.h"

static const char *TAG = "reset_btn";

// Which pin the BOOT button lives on is board-specific — see board.h.
// Every supported target defines RESET_BUTTON_GPIO. It is always an
// active-low input with a weak pull-up.

// Poll every 50 ms. That's fast enough for human perception of a "press"
// and slow enough that mechanical bounce has fully settled between samples.
#define POLL_INTERVAL_MS      50

// Hold this long to trigger. Matches the README's UX promise.
#define HOLD_THRESHOLD_MS     5000

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void reset_button_task(void *arg)
{
    (void)arg;

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << RESET_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    reset_btn_sm_t sm;
    const reset_btn_sm_config_t sm_cfg = {
        .hold_threshold_ms = HOLD_THRESHOLD_MS,
    };
    reset_btn_sm_init(&sm, &sm_cfg);

    ESP_LOGI(TAG,
             "reset button watcher started on GPIO%d (hold %d ms to clear wifi creds)",
             RESET_BUTTON_GPIO, HOLD_THRESHOLD_MS);

    bool announce_press = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));

        // Active-low: button pressed when the pad reads 0.
        const bool pressed = gpio_get_level(RESET_BUTTON_GPIO) == 0;

        // Give the operator some feedback on serial the moment the press
        // starts, so they know the timer is running. One log per press.
        if (pressed && !announce_press) {
            ESP_LOGW(TAG, "button pressed — keep holding for %d seconds to erase wifi creds",
                     HOLD_THRESHOLD_MS / 1000);
            announce_press = true;
        } else if (!pressed && announce_press) {
            announce_press = false;
        }

        if (reset_btn_sm_update(&sm, pressed, now_ms())) {
            ESP_LOGW(TAG, "hold threshold crossed — clearing wifi credentials and rebooting");
            esp_err_t err = config_clear_wifi();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "config_clear_wifi failed: %s — rebooting anyway",
                         esp_err_to_name(err));
            }
            // Let the log line flush over USB Serial/JTAG before we reset.
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }
}

void reset_button_start(void)
{
    // 4 KB stack is plenty — this task does nothing but poll + log.
    xTaskCreate(reset_button_task, "reset_btn", 4096, NULL, 3, NULL);
}
