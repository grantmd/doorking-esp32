// WiFi bring-up for the doorking bridge.
//
// Two modes, chosen at boot from the NVS config:
//
//   config_has_wifi()  -> STA mode, auto-reconnect on disconnect.
//   else               -> AP mode. Broadcasts an open "doorking-setup" SSID
//                         and serves a tiny provisioning form on port 80 at
//                         http://192.168.4.1/. Submitting the form writes
//                         new credentials + a fresh bearer token to NVS and
//                         reboots into STA mode.
//
// Must be called after nvs_flash_init and config_load.

#pragma once

#include <stdbool.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start WiFi in the appropriate mode. Returns once the driver is started;
// STA connection / AP client registration happen asynchronously via events.
void wifi_start(const doorking_config_t *cfg);

// True once the station has acquired an IP at least once.
bool wifi_sta_got_ip(void);

#ifdef __cplusplus
}
#endif
