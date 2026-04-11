#include "reset_btn_sm.h"

#include <string.h>

void reset_btn_sm_init(reset_btn_sm_t *sm, const reset_btn_sm_config_t *config)
{
    memset(sm, 0, sizeof(*sm));
    sm->config = *config;
}

bool reset_btn_sm_update(reset_btn_sm_t *sm, bool pressed, uint64_t now_ms)
{
    if (!pressed) {
        // Released. Drop the press state and re-arm the one-shot latch so
        // the next press starts fresh.
        sm->prev_pressed = false;
        sm->fired = false;
        return false;
    }

    // pressed == true
    if (!sm->prev_pressed) {
        // First sample of a new press.
        sm->prev_pressed    = true;
        sm->press_start_ms  = now_ms;
        return false;
    }

    // Still held.
    if (sm->fired) {
        return false;  // we already reported the trigger for this press
    }
    if ((now_ms - sm->press_start_ms) >= sm->config.hold_threshold_ms) {
        sm->fired = true;
        return true;
    }
    return false;
}
