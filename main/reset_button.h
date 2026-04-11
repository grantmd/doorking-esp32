// FreeRTOS task that watches the XIAO ESP32-C3 BOOT button (GPIO9). When
// the button is held continuously for 5 seconds, clear WiFi credentials
// and the auth token from NVS and reboot so the device falls back into
// AP provisioning mode.
//
// GPIO9 is a strapping pin: holding it LOW during power-on puts the chip
// into the ROM download mode instead of running our firmware. That means
// this reset path only works on a "hold BOOT while the device is already
// running" press, which is fine — and arguably more discoverable.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Spawn the reset-button watcher task. Non-blocking; returns immediately.
// Call after nvs_flash_init so config_clear_wifi() is usable on trigger.
void reset_button_start(void);

#ifdef __cplusplus
}
#endif
