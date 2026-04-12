#include "status_led.h"

#include "board.h"

#ifdef STATUS_LED_GPIO

#include "esp_log.h"
#include "led_strip.h"

static const char *TAG = "status_led";

static led_strip_handle_t s_strip = NULL;

// Dim brightness — a full-brightness WS2812 in a dark enclosure is
// blinding. 24/255 is visible but not obnoxious.
#define DIM 24

void status_led_init(void)
{
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num   = STATUS_LED_GPIO,
        .max_leds         = 1,
        .led_model        = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,   // 10 MHz — standard for WS2812
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip));
    led_strip_clear(s_strip);

    ESP_LOGI(TAG, "ws2812 status led on GPIO%d", STATUS_LED_GPIO);
}

void status_led_set_state(status_led_state_t state)
{
    if (!s_strip) {
        return;
    }

    uint8_t r = 0, g = 0, b = 0;
    switch (state) {
    case STATUS_LED_OFF:
        break;
    case STATUS_LED_AP_MODE:
        b = DIM;                       // blue
        break;
    case STATUS_LED_WIFI_CONNECTING:
        r = DIM; g = DIM / 2;         // yellow-ish
        break;
    case STATUS_LED_WIFI_CONNECTED:
        g = DIM;                       // green
        break;
    case STATUS_LED_WIFI_LOST:
        r = DIM;                       // red
        break;
    case STATUS_LED_OTA_IN_PROGRESS:
        r = DIM; b = DIM;             // magenta
        break;
    }

    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

#else  // no STATUS_LED_GPIO — compile as no-ops

void status_led_init(void) {}
void status_led_set_state(status_led_state_t state) { (void)state; }

#endif
