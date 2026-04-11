// Host-side unit tests for the reset-button debounce state machine.
// Build and run with test/run_tests.sh.

#include <stdio.h>

#include "reset_btn_sm.h"

static int         g_failures = 0;
static const char *g_current_test = "(none)";

#define CHECK(cond) do {                                                    \
    if (!(cond)) {                                                          \
        fprintf(stderr, "  FAIL %s:%d in %s: %s\n",                         \
                __FILE__, __LINE__, g_current_test, #cond);                 \
        g_failures++;                                                       \
    }                                                                       \
} while (0)

#define RUN(test) do {                                                      \
    g_current_test = #test;                                                 \
    printf("  %s\n", #test);                                                \
    test();                                                                 \
} while (0)

static const reset_btn_sm_config_t default_cfg = {
    .hold_threshold_ms = 5000,
};

static void test_unpressed_never_fires(void)
{
    reset_btn_sm_t sm;
    reset_btn_sm_init(&sm, &default_cfg);
    for (uint64_t t = 0; t < 10000; t += 50) {
        CHECK(reset_btn_sm_update(&sm, false, t) == false);
    }
}

static void test_press_under_threshold_does_not_fire(void)
{
    reset_btn_sm_t sm;
    reset_btn_sm_init(&sm, &default_cfg);
    CHECK(reset_btn_sm_update(&sm, true, 1000) == false);
    CHECK(reset_btn_sm_update(&sm, true, 3000) == false);
    CHECK(reset_btn_sm_update(&sm, true, 5999) == false);
}

static void test_press_at_threshold_fires_exactly_once(void)
{
    reset_btn_sm_t sm;
    reset_btn_sm_init(&sm, &default_cfg);
    CHECK(reset_btn_sm_update(&sm, true, 1000) == false); // press starts
    CHECK(reset_btn_sm_update(&sm, true, 2000) == false);
    CHECK(reset_btn_sm_update(&sm, true, 6000) == true);  // exactly 5000 ms later
    CHECK(reset_btn_sm_update(&sm, true, 6050) == false); // still held, already fired
    CHECK(reset_btn_sm_update(&sm, true, 9999) == false);
}

static void test_press_past_threshold_fires_on_threshold_sample(void)
{
    reset_btn_sm_t sm;
    reset_btn_sm_init(&sm, &default_cfg);
    reset_btn_sm_update(&sm, true, 0);
    // Skip straight past the threshold in one sample (e.g. the caller was
    // blocked for a while). The first sample that crosses the line fires.
    CHECK(reset_btn_sm_update(&sm, true, 8000) == true);
    CHECK(reset_btn_sm_update(&sm, true, 8001) == false);
}

static void test_short_press_does_not_fire_and_re_arms(void)
{
    reset_btn_sm_t sm;
    reset_btn_sm_init(&sm, &default_cfg);
    reset_btn_sm_update(&sm, true, 0);     // press
    reset_btn_sm_update(&sm, true, 2000);  // still pressed at 2s
    reset_btn_sm_update(&sm, false, 2050); // released before threshold
    // New press should start a fresh window.
    reset_btn_sm_update(&sm, true, 3000);
    CHECK(reset_btn_sm_update(&sm, true, 7999) == false); // not yet
    CHECK(reset_btn_sm_update(&sm, true, 8000) == true);  // 5000 ms after press
}

static void test_release_after_fire_rearms_for_next_hold(void)
{
    reset_btn_sm_t sm;
    reset_btn_sm_init(&sm, &default_cfg);
    reset_btn_sm_update(&sm, true, 0);
    CHECK(reset_btn_sm_update(&sm, true, 5000) == true);
    // User releases.
    reset_btn_sm_update(&sm, false, 5100);
    // Second hold should also fire once.
    reset_btn_sm_update(&sm, true, 6000);
    CHECK(reset_btn_sm_update(&sm, true, 11000) == true);
}

static void test_many_short_presses_never_fire(void)
{
    reset_btn_sm_t sm;
    reset_btn_sm_init(&sm, &default_cfg);
    uint64_t t = 0;
    for (int i = 0; i < 20; i++) {
        // 500 ms press, 500 ms release
        CHECK(reset_btn_sm_update(&sm, true,  t) == false); t += 250;
        CHECK(reset_btn_sm_update(&sm, true,  t) == false); t += 250;
        CHECK(reset_btn_sm_update(&sm, false, t) == false); t += 500;
    }
}

int main(void)
{
    printf("Running reset_btn_sm host tests...\n");

    RUN(test_unpressed_never_fires);
    RUN(test_press_under_threshold_does_not_fire);
    RUN(test_press_at_threshold_fires_exactly_once);
    RUN(test_press_past_threshold_fires_on_threshold_sample);
    RUN(test_short_press_does_not_fire_and_re_arms);
    RUN(test_release_after_fire_rearms_for_next_hold);
    RUN(test_many_short_presses_never_fire);

    if (g_failures > 0) {
        fprintf(stderr, "\n%d test(s) failed.\n", g_failures);
        return 1;
    }
    printf("\nAll tests passed.\n");
    return 0;
}
