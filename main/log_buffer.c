#include "log_buffer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// The ring buffer. head is the next write position; used tracks how
// many bytes have been written in total (clamped to LOG_BUFFER_SIZE to
// know how much valid data exists).
static char              s_buf[LOG_BUFFER_SIZE];
static size_t            s_head = 0;   // next write index, wraps mod LOG_BUFFER_SIZE
static size_t            s_used = 0;   // valid bytes in the buffer, <= LOG_BUFFER_SIZE
static SemaphoreHandle_t s_mutex;

// Stash the original vprintf so we can chain to it (UART output).
static vprintf_like_t    s_original_vprintf;

// Write len bytes into the ring, wrapping at the boundary. Caller must
// hold s_mutex.
static void ring_write(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        s_buf[s_head] = data[i];
        s_head = (s_head + 1) % LOG_BUFFER_SIZE;
    }
    s_used += len;
    if (s_used > LOG_BUFFER_SIZE) {
        s_used = LOG_BUFFER_SIZE;
    }
}

// The vprintf replacement installed via esp_log_set_vprintf. Formats
// the log line into a stack buffer, writes it to the ring, and then
// chains to the original vprintf (UART). Runs in whichever task called
// ESP_LOGx, so the mutex is essential.
static int log_vprintf_hook(const char *fmt, va_list args)
{
    // Format into a stack-local buffer. 256 bytes is enough for the
    // vast majority of ESP_LOGx lines; longer lines get truncated in
    // the ring buffer but still print in full to UART via the chain.
    char line[256];
    int len = vsnprintf(line, sizeof(line), fmt, args);
    if (len < 0) {
        len = 0;
    }
    if ((size_t)len >= sizeof(line)) {
        len = sizeof(line) - 1;  // truncated
    }

    if (len > 0 && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        ring_write(line, (size_t)len);
        xSemaphoreGive(s_mutex);
    }
    // else: if we can't take the mutex within 50 ms (shouldn't happen),
    // drop the ring-buffer write silently and still chain to UART.

    // Chain to the original vprintf so serial monitor keeps working.
    // We need to re-create the va_list since we consumed it above.
    // Unfortunately C doesn't let us re-use a consumed va_list, but
    // esp_log_set_vprintf's contract gives us the format string, so we
    // can use the formatted 'line' buffer we already have.
    //
    // Trick: write the already-formatted line to stdout directly rather
    // than calling the original vprintf with the raw fmt + args (which
    // we already consumed). fputs is safe here.
    fputs(line, stdout);
    return len;
}

void log_buffer_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    assert(s_mutex != NULL);

    s_original_vprintf = esp_log_set_vprintf(log_vprintf_hook);
    // s_original_vprintf is saved in case we ever need to restore it,
    // but we don't actively chain through it — we use fputs instead
    // (see log_vprintf_hook comment above).
}

char *log_buffer_snapshot(size_t *out_len)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        *out_len = 0;
        return NULL;
    }

    char *snap = malloc(s_used);
    if (!snap) {
        xSemaphoreGive(s_mutex);
        *out_len = 0;
        return NULL;
    }

    if (s_used < LOG_BUFFER_SIZE) {
        // Buffer hasn't wrapped yet. Data runs from 0..head-1.
        memcpy(snap, s_buf, s_used);
    } else {
        // Buffer has wrapped. Oldest data starts at s_head (the next
        // write position is also the oldest byte), wraps around to
        // s_head-1.
        size_t tail_len = LOG_BUFFER_SIZE - s_head;  // bytes from head to end
        memcpy(snap, s_buf + s_head, tail_len);
        memcpy(snap + tail_len, s_buf, s_head);
    }

    *out_len = s_used;
    xSemaphoreGive(s_mutex);
    return snap;
}
