// Gate state machine for the DoorKing 4602-010 bridge.
//
// Pure C with no ESP-IDF dependencies so it can be unit-tested on the host.
// The caller is responsible for driving hardware (GPIO pulses to the SSRs,
// reading the status GPIO) and providing a monotonic millisecond clock.
//
// Status sensing limitation
// -------------------------
// The 4602-010's built-in dry relay (pins 15-16) only exposes a single bit:
// "gate is at the fully-open position" vs. "gate is not at the fully-open
// position." There is no fully-closed sensor. So:
//
//   status GPIO HIGH  -> unambiguously GATE_OPEN
//   status GPIO LOW   -> one of CLOSED / OPENING / CLOSING / STOPPED, which
//                        we disambiguate with command history and a travel
//                        timer.
//
// This has one consequence callers need to understand: if the gate stops
// partway through closing (e.g. physical obstruction below the reverse-sensor
// threshold) we will eventually time out to GATE_CLOSED even though it
// isn't. Adding a reed switch at the closed stop in a later phase would
// remove this ambiguity.
//
// Usage
// -----
//   gate_sm_t sm;
//   gate_sm_config_t cfg = {
//       .travel_timeout_ms  = 30000,
//       .min_cmd_spacing_ms = 2000,
//   };
//   gate_sm_init(&sm, &cfg);
//
//   // Seed with the current status GPIO once at boot:
//   gate_sm_on_status_change(&sm, read_status_gpio(), now_ms());
//
//   // On every status GPIO edge:
//   gate_sm_on_status_change(&sm, read_status_gpio(), now_ms());
//
//   // On HTTP POST /open:
//   if (gate_sm_on_cmd_open(&sm, now_ms()) == GATE_CMD_RESULT_ACCEPTED) {
//       hw_pulse_open();
//   }
//
//   // Periodic tick (e.g. every 100 ms) to drive travel timeouts:
//   gate_sm_on_tick(&sm, now_ms());

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GATE_UNKNOWN = 0,
    GATE_CLOSED,
    GATE_OPENING,
    GATE_OPEN,
    GATE_CLOSING,
    GATE_STOPPED,
} gate_state_t;

typedef enum {
    GATE_LAST_CMD_NONE = 0,
    GATE_LAST_CMD_OPEN,
    GATE_LAST_CMD_CLOSE,
} gate_last_cmd_t;

typedef enum {
    // Command was valid for the current state; caller SHOULD pulse the
    // corresponding GPIO to actually move the gate.
    GATE_CMD_RESULT_ACCEPTED = 0,
    // Gate is already in (or moving toward) the requested state. Caller
    // SHOULD NOT pulse the GPIO. HTTP layer should still return 200.
    GATE_CMD_RESULT_IDEMPOTENT,
    // Command came too soon after the previous accepted command. Caller
    // SHOULD reject the request (e.g. HTTP 409).
    GATE_CMD_RESULT_THROTTLED,
} gate_cmd_result_t;

typedef struct {
    uint32_t travel_timeout_ms;   // max expected open/close travel duration
    uint32_t min_cmd_spacing_ms;  // minimum spacing between accepted commands
} gate_sm_config_t;

typedef struct {
    gate_state_t     state;
    gate_last_cmd_t  last_cmd;
    uint64_t         last_cmd_ms;     // when last *accepted* command was issued
    uint64_t         travel_start_ms; // when the current travel leg began
    gate_sm_config_t config;
} gate_sm_t;

// Initialise a gate_sm_t with the given config. Copies the config.
void gate_sm_init(gate_sm_t *sm, const gate_sm_config_t *config);

// Current state. Safe from any context.
gate_state_t gate_sm_state(const gate_sm_t *sm);

// Human-readable name for a state. Never NULL.
const char *gate_sm_state_name(gate_state_t state);

// Request an OPEN command. Returns ACCEPTED, IDEMPOTENT, or THROTTLED.
// On ACCEPTED, the caller must pulse the OPEN GPIO to actually move the gate.
gate_cmd_result_t gate_sm_on_cmd_open(gate_sm_t *sm, uint64_t now_ms);

// Request a CLOSE command. Same semantics as gate_sm_on_cmd_open.
gate_cmd_result_t gate_sm_on_cmd_close(gate_sm_t *sm, uint64_t now_ms);

// Feed an edge from the 4602-010 dry-relay "fully open" status line.
// Pass true when the contact is closed (gate is fully open), false otherwise.
// Also call this once at boot with the initial GPIO reading.
void gate_sm_on_status_change(gate_sm_t *sm, bool is_fully_open, uint64_t now_ms);

// Periodic tick to drive the travel timeout. Safe to call as often as you
// like; internally it only acts when in a moving state and the timeout has
// elapsed.
void gate_sm_on_tick(gate_sm_t *sm, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
