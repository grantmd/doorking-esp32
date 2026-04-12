// Status LED driver for boards with a WS2812 RGB LED.
//
// Maps high-level WiFi / device states to solid colors so a glance at
// the LED tells you what mode the device is in without a serial monitor
// or an HTTP request. On boards that don't have a user-controllable LED
// (e.g. the XIAO ESP32-C3), all functions compile as no-ops.
//
// Colours:
//   blue    — AP provisioning mode, waiting for WiFi setup
//   yellow  — STA mode, connecting to WiFi
//   green   — STA mode, connected and ready
//   red     — STA mode, WiFi lost (auto-reconnecting)

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_OFF,
    STATUS_LED_AP_MODE,         // blue
    STATUS_LED_WIFI_CONNECTING, // yellow
    STATUS_LED_WIFI_CONNECTED,  // green
    STATUS_LED_WIFI_LOST,       // red
} status_led_state_t;

// Initialise the WS2812 LED strip driver. No-op on boards without
// STATUS_LED_GPIO defined in board.h. Call once, early in app_main.
void status_led_init(void);

// Set the LED to a high-level state. No-op on boards without a LED.
void status_led_set_state(status_led_state_t state);

#ifdef __cplusplus
}
#endif
