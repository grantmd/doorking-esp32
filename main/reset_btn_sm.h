// Debounce + hold-threshold state machine for a "press and hold to factory
// reset" button.
//
// Pure C with no ESP-IDF dependencies so it can be unit-tested on the host.
// The caller is responsible for reading the physical button (with whatever
// polarity / pull-up their board needs) and providing a monotonic
// millisecond clock. The FreeRTOS wrapper lives in reset_button.c.
//
// Behaviour
// ---------
//   * An unpressed button makes update() return false and re-arms the
//     one-shot latch.
//   * On the first observed press we record press_start_ms. Subsequent
//     updates return false until the hold duration >= hold_threshold_ms,
//     at which point update() returns true EXACTLY ONCE for that press.
//   * Further updates during the same continuous press return false (the
//     caller already handled the trigger).
//   * Releasing and re-pressing starts a fresh hold window.
//
// A 50 ms poll interval in the caller gives effectively free debouncing:
// any mechanical contact bounce settles long before the next sample.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t hold_threshold_ms;   // how long the button must be held to fire
} reset_btn_sm_config_t;

typedef struct {
    reset_btn_sm_config_t config;
    bool      prev_pressed;       // last observed physical state
    uint64_t  press_start_ms;     // when the current press started (valid iff prev_pressed)
    bool      fired;              // one-shot latch, cleared on release
} reset_btn_sm_t;

// Initialise the state machine. Copies the config.
void reset_btn_sm_init(reset_btn_sm_t *sm, const reset_btn_sm_config_t *config);

// Feed the current pressed state and a timestamp. Returns true exactly
// once per press: the first call where the hold threshold has been
// reached. Returns false in every other case (not pressed, still waiting,
// or already fired for this press).
bool reset_btn_sm_update(reset_btn_sm_t *sm, bool pressed, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
