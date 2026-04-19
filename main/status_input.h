// 4602-010 "fully open" status input.
//
// Watches the GPIO wired to the 4602-010's configurable dry relay
// (pin 15 ↔ 18, set to "fully open" via SW1 switches 4=OFF, 5=OFF).
// Contact closed → GPIO LOW (via internal pull-up) → gate is fully open.
// Contact open → GPIO HIGH → gate is NOT fully open.
//
// The DKS dry relay is mechanical; debounce with 3 consecutive matching
// reads at 50 ms intervals (150 ms total) to filter chatter. Real gate
// transitions happen on the order of seconds, so 150 ms latency is
// imperceptible.
//
// On every accepted edge, and once at boot with the initial reading,
// calls gate_sm_on_status_change. The state machine seeds itself from
// UNKNOWN based on the first reading (see gate_sm.c), then tracks real
// edges the 4602-010 emits as the gate moves.
//
// Pin wiring
// ----------
// Dev board STATUS_INPUT_GPIO  →  4602-010 pin 15
// Dev board GND                →  4602-010 pin 18 (Low Voltage Common)
//
// The contact is a dry relay, already galvanically isolated — no
// opto-isolator required for short cable runs inside the operator
// enclosure.

#pragma once

#include "gate_sm.h"

#ifdef __cplusplus
extern "C" {
#endif

// Spawn the status-input watcher task. Non-blocking; returns immediately.
// Takes the gate state machine whose gate_sm_on_status_change will be
// called on every debounced edge. Retains the pointer for the lifetime
// of the task.
//
// Compiles to a no-op if the current target doesn't define
// STATUS_INPUT_GPIO in board.h (currently all targets define it, but
// keeping the pattern consistent with status_led).
void status_input_start(gate_sm_t *sm);

#ifdef __cplusplus
}
#endif
