#include "status_input.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"

static const char *TAG = "status_input";

// Poll every 50 ms. The DKS dry relay is mechanical but transitions on
// the order of seconds, so 50 ms sampling is more than fast enough.
#define POLL_INTERVAL_MS    50

// Accept an edge only after this many consecutive reads agree. 3 × 50 ms
// = 150 ms of stability, which filters any mechanical chatter without
// meaningfully delaying the state update.
#define DEBOUNCE_SAMPLES    3

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void status_input_task(void *arg)
{
    gate_sm_t *sm = (gate_sm_t *)arg;

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << STATUS_INPUT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    // Active-low: contact closed (gate fully open) → GPIO reads 0.
    // Take the initial reading once and seed the state machine with it.
    // gate_sm_on_status_change handles the UNKNOWN → CLOSED / OPEN
    // transition that this first call triggers.
    bool stable_open = (gpio_get_level(STATUS_INPUT_GPIO) == 0);
    gate_sm_on_status_change(sm, stable_open, now_ms());

    ESP_LOGI(TAG, "status input watcher started on GPIO%d (initial: %s)",
             STATUS_INPUT_GPIO,
             stable_open ? "fully open" : "not fully open");

    // Track a candidate edge: once the raw reading differs from the
    // current stable state, count consecutive agreeing samples until
    // DEBOUNCE_SAMPLES is reached, then accept the edge.
    int candidate_count = 0;
    bool candidate_open = stable_open;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));

        const bool raw_open = (gpio_get_level(STATUS_INPUT_GPIO) == 0);

        if (raw_open == stable_open) {
            // No change — reset any in-progress debounce.
            candidate_count = 0;
            continue;
        }

        if (raw_open != candidate_open) {
            // Raw reading disagrees with the in-progress candidate —
            // restart the debounce window with this new candidate.
            candidate_open = raw_open;
            candidate_count = 1;
            continue;
        }

        candidate_count++;
        if (candidate_count < DEBOUNCE_SAMPLES) {
            continue;
        }

        // Edge confirmed.
        stable_open = candidate_open;
        candidate_count = 0;
        ESP_LOGI(TAG, "edge: gate is %s",
                 stable_open ? "fully open" : "not fully open");
        gate_sm_on_status_change(sm, stable_open, now_ms());
    }
}

void status_input_start(gate_sm_t *sm)
{
    // 4 KB stack matches reset_button.c — this task does nothing but
    // poll a GPIO, log, and call into gate_sm.
    xTaskCreate(status_input_task, "status_in", 4096, sm, 3, NULL);
}
