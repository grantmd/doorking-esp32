// Main HTTP API server for the doorking bridge.
//
// Runs in STA mode only (not in AP provisioning mode — the provisioning
// server in wifi.c handles that). Hosts bearer-authed endpoints for gate
// control, status, and diagnostics. Started from app_main after WiFi
// is up.
//
// Endpoints:
//   GET  /                    -> dashboard HTML                        (no auth)
//   GET  /health              -> liveness + version JSON               (no auth)
//   GET  /logs                -> ring buffer text/plain dump           (bearer auth)
//   GET  /status              -> gate state + last command JSON        (bearer auth)
//   POST /open                -> command gate open, pulse OPEN relay   (bearer auth)
//   POST /close               -> command gate close, pulse CLOSE relay (bearer auth)
//   POST /update              -> push OTA firmware upload              (bearer auth)
//   POST /update/check        -> check GitHub for new release          (bearer auth)
//   POST /update/pull         -> trigger pull OTA from GitHub          (bearer auth)
//   POST /reboot              -> restart the device                    (bearer auth)
//   GET  /i2c/scan            -> JSON list of ACK'd I²C addresses      (bearer auth)
//   POST /i2c/relay-address   -> reassign Qwiic Single Relay address   (bearer auth)

#pragma once

#include "config.h"
#include "driver/i2c_master.h"
#include "gate_sm.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the main HTTP API server on port 80. Only call this when WiFi
// is in STA mode and we have an IP. Does nothing if the server is
// already running (safe to call multiple times).
//
// The gate_sm pointer is retained for the lifetime of the server and
// accessed from the httpd task on every /status, /open, /close request.
// The caller's tick loop also calls gate_sm_on_tick from the app_main
// task. On a single-core chip (C3, C5) or a low-contention dual-core
// chip (WROOM) with sub-microsecond state assignments and requests
// arriving at most a few times per day, the data race is benign. Add a
// mutex here when relay pulse timing makes the race window relevant.
void http_api_start(const doorking_config_t *cfg,
                    gate_sm_t *sm,
                    i2c_master_bus_handle_t i2c_bus);

#ifdef __cplusplus
}
#endif
