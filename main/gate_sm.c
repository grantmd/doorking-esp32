#include "gate_sm.h"

#include <string.h>

static bool cmd_spacing_ok(const gate_sm_t *sm, uint64_t now_ms)
{
    // First command after boot: last_cmd_ms is 0, allow it through.
    if (sm->last_cmd == GATE_LAST_CMD_NONE) {
        return true;
    }
    return (now_ms - sm->last_cmd_ms) >= sm->config.min_cmd_spacing_ms;
}

void gate_sm_init(gate_sm_t *sm, const gate_sm_config_t *config)
{
    memset(sm, 0, sizeof(*sm));
    sm->state = GATE_UNKNOWN;
    sm->last_cmd = GATE_LAST_CMD_NONE;
    sm->config = *config;
}

gate_state_t gate_sm_state(const gate_sm_t *sm)
{
    return sm->state;
}

const char *gate_sm_state_name(gate_state_t state)
{
    switch (state) {
        case GATE_UNKNOWN: return "UNKNOWN";
        case GATE_CLOSED:  return "CLOSED";
        case GATE_OPENING: return "OPENING";
        case GATE_OPEN:    return "OPEN";
        case GATE_CLOSING: return "CLOSING";
        case GATE_STOPPED: return "STOPPED";
    }
    return "INVALID";
}

gate_last_cmd_t gate_sm_last_cmd(const gate_sm_t *sm)
{
    return sm->last_cmd;
}

const char *gate_sm_last_cmd_name(gate_last_cmd_t cmd)
{
    switch (cmd) {
        case GATE_LAST_CMD_NONE:  return "none";
        case GATE_LAST_CMD_OPEN:  return "open";
        case GATE_LAST_CMD_CLOSE: return "close";
    }
    return "unknown";
}

uint64_t gate_sm_last_cmd_ms(const gate_sm_t *sm)
{
    return sm->last_cmd_ms;
}

gate_cmd_result_t gate_sm_on_cmd_open(gate_sm_t *sm, uint64_t now_ms)
{
    // Idempotent if we're already there or already heading there.
    if (sm->state == GATE_OPEN || sm->state == GATE_OPENING) {
        return GATE_CMD_RESULT_IDEMPOTENT;
    }
    if (!cmd_spacing_ok(sm, now_ms)) {
        return GATE_CMD_RESULT_THROTTLED;
    }
    sm->state = GATE_OPENING;
    sm->last_cmd = GATE_LAST_CMD_OPEN;
    sm->last_cmd_ms = now_ms;
    sm->travel_start_ms = now_ms;
    return GATE_CMD_RESULT_ACCEPTED;
}

gate_cmd_result_t gate_sm_on_cmd_close(gate_sm_t *sm, uint64_t now_ms)
{
    if (sm->state == GATE_CLOSED || sm->state == GATE_CLOSING) {
        return GATE_CMD_RESULT_IDEMPOTENT;
    }
    if (!cmd_spacing_ok(sm, now_ms)) {
        return GATE_CMD_RESULT_THROTTLED;
    }
    sm->state = GATE_CLOSING;
    sm->last_cmd = GATE_LAST_CMD_CLOSE;
    sm->last_cmd_ms = now_ms;
    sm->travel_start_ms = now_ms;
    return GATE_CMD_RESULT_ACCEPTED;
}

void gate_sm_on_status_change(gate_sm_t *sm, bool is_fully_open, uint64_t now_ms)
{
    if (is_fully_open) {
        // Unambiguous: the dry-relay contact is closed, gate is fully open.
        sm->state = GATE_OPEN;
        return;
    }

    // is_fully_open == false: the gate is NOT at the fully-open position.
    // Interpret the event based on where we thought we were.
    switch (sm->state) {
        case GATE_UNKNOWN:
            // First status read at boot, contact is open. We have no
            // fully-closed sensor, so assume CLOSED as the most likely
            // resting state.
            sm->state = GATE_CLOSED;
            break;

        case GATE_OPEN:
            // The gate was fully open and just left that position. Either we
            // commanded a close or something else did (wired keypad, remote,
            // Chamberlain keypad). Either way it is now closing.
            sm->state = GATE_CLOSING;
            sm->travel_start_ms = now_ms;
            break;

        case GATE_OPENING:
        case GATE_CLOSING:
        case GATE_CLOSED:
        case GATE_STOPPED:
            // Already not fully open. No transition on this event.
            break;
    }
}

void gate_sm_on_tick(gate_sm_t *sm, uint64_t now_ms)
{
    if (sm->state != GATE_OPENING && sm->state != GATE_CLOSING) {
        return;
    }
    if ((now_ms - sm->travel_start_ms) < sm->config.travel_timeout_ms) {
        return;
    }

    // Travel timer expired.
    if (sm->state == GATE_OPENING) {
        // We expected the status GPIO to rise to "fully open" within the
        // travel window and it didn't. Something is wrong — obstruction,
        // power loss, controller fault. Surface STOPPED to the caller.
        sm->state = GATE_STOPPED;
    } else {
        // Closing: no sensor can confirm we reached the closed stop. Assume
        // we did. If the gate reversed mid-close due to the inherent reverse
        // sensor, gate_sm_on_status_change would already have moved us back
        // to OPEN before this timeout fired.
        sm->state = GATE_CLOSED;
    }
}
