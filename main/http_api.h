// Main HTTP API server for the doorking bridge.
//
// Runs in STA mode only (not in AP provisioning mode — the provisioning
// server in wifi.c handles that). Hosts bearer-authed endpoints for gate
// control, status, and diagnostics. Started from app_main after WiFi
// is up.
//
// Endpoints served today:
//   GET  /health  -> {"ok":true}                  (no auth)
//   GET  /logs    -> ring buffer text/plain dump   (bearer auth)
//
// Planned:
//   GET  /status  -> gate state JSON               (bearer auth)
//   POST /open    -> pulse OPEN relay               (bearer auth)
//   POST /close   -> pulse CLOSE relay              (bearer auth)

#pragma once

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the main HTTP API server on port 80. Only call this when WiFi
// is in STA mode and we have an IP. Does nothing if the server is
// already running (safe to call multiple times).
void http_api_start(const doorking_config_t *cfg);

#ifdef __cplusplus
}
#endif
