// Circular RAM log buffer that captures all ESP_LOGx output.
//
// Initialised early in app_main (before WiFi, before config) so that
// boot-time log lines are captured. Internally hooks into ESP-IDF's
// logging system via esp_log_set_vprintf; the hook tees every formatted
// log line to both the ring buffer AND the original UART output so the
// serial monitor continues to work when USB is attached.
//
// The buffer is a fixed 16 KB byte ring. When full, new lines silently
// overwrite the oldest data. A mutex protects concurrent access from the
// logging hook (called from any task) and the HTTP handler that reads
// the buffer.
//
// Usage:
//   log_buffer_init();          // once, early in app_main
//   ...
//   // Later, in an HTTP handler:
//   size_t len;
//   char *snapshot = log_buffer_snapshot(&len);  // malloc'd copy
//   if (snapshot) {
//       httpd_resp_send(req, snapshot, len);
//       free(snapshot);
//   }

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Size of the ring buffer in bytes. 16 KB holds several hours of
// event-driven gate-controller logs (state transitions, commands, WiFi
// events, errors) once heartbeat noise is demoted to ESP_LOGD.
#define LOG_BUFFER_SIZE (16 * 1024)

// Allocate the ring buffer and install the esp_log_set_vprintf hook.
// Safe to call exactly once. Must be called before any ESP_LOGx output
// you want captured (i.e. before nvs_flash_init, before wifi_start).
void log_buffer_init(void);

// Return a malloc'd snapshot of the current buffer contents in
// chronological order (oldest byte first). Caller must free the
// returned pointer. Sets *out_len to the number of valid bytes.
// Returns NULL on allocation failure.
char *log_buffer_snapshot(size_t *out_len);

#ifdef __cplusplus
}
#endif
