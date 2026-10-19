/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_dap_breakpoints.c - Unit tests for DAP breakpoint data structures
 *
 * Tests breakpoint/watch data structure operations without full runtime.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../src/app/dap/xdap_debug.h"
#include "../test_win_compat.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)                                                                             \
    do {                                                                                           \
        printf("  Testing %s... ", #name);                                                         \
        test_##name();                                                                             \
        printf("PASS\n");                                                                          \
        tests_passed++;                                                                            \
    } while (0)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL at line %d: %s\n", __LINE__, #cond);                                      \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_STR_EQ(a, b) ASSERT(strcmp((a), (b)) == 0)

// ============================================================================
// Debug State Structure Tests (without isolate)
// ============================================================================

// Create a standalone debug state for testing
static XrDebugState *create_debug_state(void) {
    XrDebugState *state = (XrDebugState *) calloc(1, sizeof(XrDebugState));
    if (state) {
        state->next_bp_id = 1;
        state->next_watch_id = 1;
        state->next_var_ref_id = 1000;
    }
    return state;
}

static void free_debug_state(XrDebugState *state) {
    if (!state)
        return;

    // Free breakpoints
    struct XrBreakpoint *bp = state->breakpoints;
    while (bp) {
        struct XrBreakpoint *next = bp->next;
        free(bp->path);
        free(bp->condition);
        free(bp->log_message);
        free(bp->hit_condition);
        free(bp);
        bp = next;
    }

    // Free watches
    struct XrWatch *w = state->watches;
    while (w) {
        struct XrWatch *next = w->next;
        free(w->expression);
        free(w);
        w = next;
    }

    free(state);
}

// Helper to add breakpoint to state
static int add_breakpoint(XrDebugState *state, const char *path, int line, const char *condition,
                          const char *log_msg, const char *hit_cond) {
    struct XrBreakpoint *bp = (struct XrBreakpoint *) calloc(1, sizeof(struct XrBreakpoint));
    if (!bp)
        return -1;

    bp->id = state->next_bp_id++;
    bp->path = strdup(path);
    bp->line = line;
    bp->enabled = true;
    if (condition)
        bp->condition = strdup(condition);
    if (log_msg)
        bp->log_message = strdup(log_msg);
    if (hit_cond)
        bp->hit_condition = strdup(hit_cond);

    bp->next = state->breakpoints;
    state->breakpoints = bp;

    return bp->id;
}

// Helper to find breakpoint
static struct XrBreakpoint *find_breakpoint(XrDebugState *state, const char *path, int line) {
    for (struct XrBreakpoint *bp = state->breakpoints; bp; bp = bp->next) {
        if (bp->line == line && strcmp(bp->path, path) == 0) {
            return bp;
        }
    }
    return NULL;
}

// Helper to add watch
static int add_watch(XrDebugState *state, const char *expression) {
    struct XrWatch *w = (struct XrWatch *) calloc(1, sizeof(struct XrWatch));
    if (!w)
        return -1;

    w->id = state->next_watch_id++;
    w->expression = strdup(expression);

    w->next = state->watches;
    state->watches = w;

    return w->id;
}

// Helper to count watches
static int count_watches(XrDebugState *state) {
    int count = 0;
    for (struct XrWatch *w = state->watches; w; w = w->next) {
        count++;
    }
    return count;
}

// ============================================================================
// Breakpoint Tests
// ============================================================================

TEST(breakpoint_add) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    int id = add_breakpoint(state, "/test/file.xr", 10, NULL, NULL, NULL);
    ASSERT(id > 0);

    struct XrBreakpoint *bp = find_breakpoint(state, "/test/file.xr", 10);
    ASSERT(bp != NULL);
    ASSERT_EQ(bp->line, 10);
    ASSERT_STR_EQ(bp->path, "/test/file.xr");

    free_debug_state(state);
}

TEST(breakpoint_add_multiple) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    int id1 = add_breakpoint(state, "/test/file.xr", 10, NULL, NULL, NULL);
    int id2 = add_breakpoint(state, "/test/file.xr", 20, NULL, NULL, NULL);
    int id3 = add_breakpoint(state, "/test/other.xr", 5, NULL, NULL, NULL);

    ASSERT(id1 > 0 && id2 > 0 && id3 > 0);
    ASSERT_NE(id1, id2);
    ASSERT_NE(id2, id3);

    ASSERT(find_breakpoint(state, "/test/file.xr", 10) != NULL);
    ASSERT(find_breakpoint(state, "/test/file.xr", 20) != NULL);
    ASSERT(find_breakpoint(state, "/test/other.xr", 5) != NULL);

    free_debug_state(state);
}

TEST(breakpoint_with_condition) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    int id = add_breakpoint(state, "/test/file.xr", 10, "x > 5", NULL, NULL);
    ASSERT(id > 0);

    struct XrBreakpoint *bp = find_breakpoint(state, "/test/file.xr", 10);
    ASSERT(bp != NULL);
    ASSERT(bp->condition != NULL);
    ASSERT_STR_EQ(bp->condition, "x > 5");

    free_debug_state(state);
}

TEST(breakpoint_logpoint) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    int id = add_breakpoint(state, "/test/file.xr", 10, NULL, "x = {x}", NULL);
    ASSERT(id > 0);

    struct XrBreakpoint *bp = find_breakpoint(state, "/test/file.xr", 10);
    ASSERT(bp != NULL);
    ASSERT(bp->log_message != NULL);
    ASSERT_STR_EQ(bp->log_message, "x = {x}");

    free_debug_state(state);
}

TEST(breakpoint_hit_condition) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    int id = add_breakpoint(state, "/test/file.xr", 10, NULL, NULL, ">5");
    ASSERT(id > 0);

    struct XrBreakpoint *bp = find_breakpoint(state, "/test/file.xr", 10);
    ASSERT(bp != NULL);
    ASSERT(bp->hit_condition != NULL);
    ASSERT_STR_EQ(bp->hit_condition, ">5");

    free_debug_state(state);
}

TEST(breakpoint_combined) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    int id = add_breakpoint(state, "/test/file.xr", 10, "y == 10", "value: {y}", "==3");
    ASSERT(id > 0);

    struct XrBreakpoint *bp = find_breakpoint(state, "/test/file.xr", 10);
    ASSERT(bp != NULL);
    ASSERT_STR_EQ(bp->condition, "y == 10");
    ASSERT_STR_EQ(bp->log_message, "value: {y}");
    ASSERT_STR_EQ(bp->hit_condition, "==3");

    free_debug_state(state);
}

TEST(breakpoint_not_found) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    add_breakpoint(state, "/test/file.xr", 10, NULL, NULL, NULL);

    ASSERT(find_breakpoint(state, "/test/file.xr", 20) == NULL);
    ASSERT(find_breakpoint(state, "/test/other.xr", 10) == NULL);

    free_debug_state(state);
}

// ============================================================================
// Watch Tests
// ============================================================================

TEST(watch_add) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    int id = add_watch(state, "x + y");
    ASSERT(id > 0);
    ASSERT_EQ(count_watches(state), 1);

    free_debug_state(state);
}

TEST(watch_add_multiple) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    int id1 = add_watch(state, "x");
    int id2 = add_watch(state, "y");
    int id3 = add_watch(state, "x + y");

    ASSERT(id1 > 0 && id2 > 0 && id3 > 0);
    ASSERT_EQ(count_watches(state), 3);

    free_debug_state(state);
}

TEST(watch_expression_stored) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    add_watch(state, "myVariable");

    ASSERT(state->watches != NULL);
    ASSERT_STR_EQ(state->watches->expression, "myVariable");

    free_debug_state(state);
}

// ============================================================================
// Debug State Tests
// ============================================================================

TEST(debug_state_initial) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    ASSERT(state->enabled == false);
    ASSERT(state->breakpoints == NULL);
    ASSERT(state->watches == NULL);
    ASSERT_EQ(state->next_bp_id, 1);
    ASSERT_EQ(state->next_watch_id, 1);

    free_debug_state(state);
}

TEST(debug_state_enable) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    state->enabled = true;
    ASSERT(state->enabled == true);

    state->enabled = false;
    ASSERT(state->enabled == false);

    free_debug_state(state);
}

TEST(debug_state_exception_breakpoints) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    state->break_on_uncaught = true;
    state->break_on_caught = false;
    ASSERT(state->break_on_uncaught == true);
    ASSERT(state->break_on_caught == false);

    state->break_on_uncaught = false;
    state->break_on_caught = true;
    ASSERT(state->break_on_uncaught == false);
    ASSERT(state->break_on_caught == true);

    free_debug_state(state);
}

TEST(debug_state_var_ref_id) {
    XrDebugState *state = create_debug_state();
    ASSERT(state != NULL);

    // Variable reference IDs start at 1000 to avoid collision with frame IDs
    ASSERT(state->next_var_ref_id >= 1000);

    free_debug_state(state);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    xr_test_suppress_dialogs();
    (void) argc;
    (void) argv;

    printf("\n=== DAP Breakpoint Data Structure Unit Tests ===\n\n");

    printf("Breakpoint tests:\n");
    RUN_TEST(breakpoint_add);
    RUN_TEST(breakpoint_add_multiple);
    RUN_TEST(breakpoint_with_condition);
    RUN_TEST(breakpoint_logpoint);
    RUN_TEST(breakpoint_hit_condition);
    RUN_TEST(breakpoint_combined);
    RUN_TEST(breakpoint_not_found);

    printf("\nWatch tests:\n");
    RUN_TEST(watch_add);
    RUN_TEST(watch_add_multiple);
    RUN_TEST(watch_expression_stored);

    printf("\nDebug state tests:\n");
    RUN_TEST(debug_state_initial);
    RUN_TEST(debug_state_enable);
    RUN_TEST(debug_state_exception_breakpoints);
    RUN_TEST(debug_state_var_ref_id);

    printf("\n=== Results: %d passed, %d failed ===\n\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
