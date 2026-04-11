// Host-side unit tests for the gate state machine.
//
// Build and run with test/run_tests.sh. No external test framework — just a
// tiny assertion macro and a main() that calls each test in sequence. Runs in
// milliseconds and is trivially debuggable under lldb.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gate_sm.h"

static int         g_failures = 0;
static const char *g_current_test = "(none)";

#define CHECK(cond) do {                                                    \
    if (!(cond)) {                                                          \
        fprintf(stderr, "  FAIL %s:%d in %s: %s\n",                         \
                __FILE__, __LINE__, g_current_test, #cond);                 \
        g_failures++;                                                       \
    }                                                                       \
} while (0)

#define CHECK_STATE(sm, expected) do {                                      \
    gate_state_t _s = gate_sm_state(sm);                                    \
    if (_s != (expected)) {                                                 \
        fprintf(stderr, "  FAIL %s:%d in %s: expected %s, got %s\n",        \
                __FILE__, __LINE__, g_current_test,                         \
                gate_sm_state_name(expected), gate_sm_state_name(_s));      \
        g_failures++;                                                       \
    }                                                                       \
} while (0)

#define RUN(test) do {                                                      \
    g_current_test = #test;                                                 \
    printf("  %s\n", #test);                                                \
    test();                                                                 \
} while (0)

static const gate_sm_config_t default_cfg = {
    .travel_timeout_ms  = 30000,
    .min_cmd_spacing_ms = 2000,
};

// --- Initialisation & seeding ----------------------------------------------

static void test_init_is_unknown(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    CHECK_STATE(&sm, GATE_UNKNOWN);
}

static void test_first_status_open_becomes_open(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, true, 1000);
    CHECK_STATE(&sm, GATE_OPEN);
}

static void test_first_status_not_open_becomes_closed(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, false, 1000);
    CHECK_STATE(&sm, GATE_CLOSED);
}

// --- Open command from various states --------------------------------------

static void test_closed_open_cmd_accepted(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, false, 0);
    gate_cmd_result_t r = gate_sm_on_cmd_open(&sm, 1000);
    CHECK(r == GATE_CMD_RESULT_ACCEPTED);
    CHECK_STATE(&sm, GATE_OPENING);
}

static void test_open_open_cmd_is_idempotent(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, true, 0);
    gate_cmd_result_t r = gate_sm_on_cmd_open(&sm, 1000);
    CHECK(r == GATE_CMD_RESULT_IDEMPOTENT);
    CHECK_STATE(&sm, GATE_OPEN);
}

static void test_opening_open_cmd_is_idempotent(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, false, 0);
    gate_sm_on_cmd_open(&sm, 1000);
    CHECK_STATE(&sm, GATE_OPENING);
    gate_cmd_result_t r = gate_sm_on_cmd_open(&sm, 5000);
    CHECK(r == GATE_CMD_RESULT_IDEMPOTENT);
    CHECK_STATE(&sm, GATE_OPENING);
}

// --- Close command from various states -------------------------------------

static void test_open_close_cmd_accepted(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, true, 0);
    gate_cmd_result_t r = gate_sm_on_cmd_close(&sm, 1000);
    CHECK(r == GATE_CMD_RESULT_ACCEPTED);
    CHECK_STATE(&sm, GATE_CLOSING);
}

static void test_closed_close_cmd_is_idempotent(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, false, 0);
    gate_cmd_result_t r = gate_sm_on_cmd_close(&sm, 1000);
    CHECK(r == GATE_CMD_RESULT_IDEMPOTENT);
    CHECK_STATE(&sm, GATE_CLOSED);
}

static void test_closing_close_cmd_is_idempotent(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, true, 0);
    gate_sm_on_cmd_close(&sm, 1000);
    CHECK_STATE(&sm, GATE_CLOSING);
    gate_cmd_result_t r = gate_sm_on_cmd_close(&sm, 5000);
    CHECK(r == GATE_CMD_RESULT_IDEMPOTENT);
    CHECK_STATE(&sm, GATE_CLOSING);
}

// --- Throttling -------------------------------------------------------------

static void test_rapid_reversal_throttled(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, true, 0);
    gate_cmd_result_t r1 = gate_sm_on_cmd_close(&sm, 1000);
    CHECK(r1 == GATE_CMD_RESULT_ACCEPTED);
    // 500 ms later, well inside the 2000 ms throttle window.
    gate_cmd_result_t r2 = gate_sm_on_cmd_open(&sm, 1500);
    CHECK(r2 == GATE_CMD_RESULT_THROTTLED);
    // Throttled commands must not mutate state.
    CHECK_STATE(&sm, GATE_CLOSING);
}

static void test_spaced_reversal_accepted(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, true, 0);
    gate_sm_on_cmd_close(&sm, 1000);
    gate_cmd_result_t r = gate_sm_on_cmd_open(&sm, 1000 + 2001);
    CHECK(r == GATE_CMD_RESULT_ACCEPTED);
    CHECK_STATE(&sm, GATE_OPENING);
}

// --- Travel timeouts --------------------------------------------------------

static void test_opening_status_rises_becomes_open(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, false, 0);
    gate_sm_on_cmd_open(&sm, 1000);
    gate_sm_on_status_change(&sm, true, 25000);
    CHECK_STATE(&sm, GATE_OPEN);
}

static void test_opening_travel_timeout_becomes_stopped(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, false, 0);
    gate_sm_on_cmd_open(&sm, 1000);
    // One tick just before timeout: still OPENING.
    gate_sm_on_tick(&sm, 1000 + 29999);
    CHECK_STATE(&sm, GATE_OPENING);
    // One tick past timeout: STOPPED.
    gate_sm_on_tick(&sm, 1000 + 30001);
    CHECK_STATE(&sm, GATE_STOPPED);
}

static void test_closing_travel_timeout_becomes_closed(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, true, 0);
    gate_sm_on_cmd_close(&sm, 1000);
    // Gate leaves fully-open position shortly after command.
    gate_sm_on_status_change(&sm, false, 1100);
    CHECK_STATE(&sm, GATE_CLOSING);
    // Travel timer measured from the command, not the status edge.
    gate_sm_on_tick(&sm, 1000 + 30001);
    CHECK_STATE(&sm, GATE_CLOSED);
}

static void test_closing_safety_reverse_becomes_open(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, true, 0);
    gate_sm_on_cmd_close(&sm, 1000);
    gate_sm_on_status_change(&sm, false, 1100);  // gate leaves fully open
    // DKS inherent reverse sensor kicks in and returns the gate to fully open.
    gate_sm_on_status_change(&sm, true, 8000);
    CHECK_STATE(&sm, GATE_OPEN);
}

static void test_tick_noop_on_resting_states(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);

    gate_sm_on_status_change(&sm, false, 0);
    gate_sm_on_tick(&sm, 9999999);
    CHECK_STATE(&sm, GATE_CLOSED);

    gate_sm_on_status_change(&sm, true, 10000000);
    gate_sm_on_tick(&sm, 20000000);
    CHECK_STATE(&sm, GATE_OPEN);
}

// --- External triggers (someone else operates the gate) --------------------

static void test_external_trigger_from_open_is_closing(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, true, 0);
    // No command from us, but the status line drops: someone hit the wired
    // keypad or the Chamberlain remote.
    gate_sm_on_status_change(&sm, false, 5000);
    CHECK_STATE(&sm, GATE_CLOSING);
}

static void test_external_trigger_from_closed_is_open(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, false, 0);
    // Later the status rises without us commanding anything.
    gate_sm_on_status_change(&sm, true, 25000);
    CHECK_STATE(&sm, GATE_OPEN);
}

// --- Recovery from STOPPED --------------------------------------------------

static void test_stopped_accepts_new_commands(void)
{
    gate_sm_t sm;
    gate_sm_init(&sm, &default_cfg);
    gate_sm_on_status_change(&sm, false, 0);
    gate_sm_on_cmd_open(&sm, 1000);
    gate_sm_on_tick(&sm, 1000 + 30001);
    CHECK_STATE(&sm, GATE_STOPPED);

    // Operator cleared the obstruction and wants to try again. Spacing must
    // be respected (30001 - 1000 = 29001 ms, well past 2000 ms throttle).
    gate_cmd_result_t r = gate_sm_on_cmd_open(&sm, 1000 + 30002);
    CHECK(r == GATE_CMD_RESULT_ACCEPTED);
    CHECK_STATE(&sm, GATE_OPENING);
}

// --- Driver ----------------------------------------------------------------

int main(void)
{
    printf("Running gate_sm host tests...\n");

    RUN(test_init_is_unknown);
    RUN(test_first_status_open_becomes_open);
    RUN(test_first_status_not_open_becomes_closed);

    RUN(test_closed_open_cmd_accepted);
    RUN(test_open_open_cmd_is_idempotent);
    RUN(test_opening_open_cmd_is_idempotent);

    RUN(test_open_close_cmd_accepted);
    RUN(test_closed_close_cmd_is_idempotent);
    RUN(test_closing_close_cmd_is_idempotent);

    RUN(test_rapid_reversal_throttled);
    RUN(test_spaced_reversal_accepted);

    RUN(test_opening_status_rises_becomes_open);
    RUN(test_opening_travel_timeout_becomes_stopped);
    RUN(test_closing_travel_timeout_becomes_closed);
    RUN(test_closing_safety_reverse_becomes_open);
    RUN(test_tick_noop_on_resting_states);

    RUN(test_external_trigger_from_open_is_closing);
    RUN(test_external_trigger_from_closed_is_open);

    RUN(test_stopped_accepts_new_commands);

    if (g_failures > 0) {
        fprintf(stderr, "\n%d test(s) failed.\n", g_failures);
        return 1;
    }
    printf("\nAll tests passed.\n");
    return 0;
}
