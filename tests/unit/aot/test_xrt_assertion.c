#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../test_win_compat.h"

#define XRT_DATA_ALIGN 32

static int g_passed;
static int g_failed;
static int g_fail_next_malloc;
static int g_fail_format_step = -1;
static void test_assertion_format_step(unsigned step);

static void *test_malloc(size_t size) {
    if (g_fail_next_malloc) {
        g_fail_next_malloc = 0;
        return NULL;
    }
    return malloc(size);
}

static void *test_calloc(size_t count, size_t size) {
    return calloc(count, size);
}

static void *test_realloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

static void test_free(void *ptr) {
    free(ptr);
}

static void *test_alloc_aligned(size_t size) {
    return xr_test_alloc_aligned(size, XRT_DATA_ALIGN);
}

static void test_free_aligned(void *ptr) {
    xr_test_free_aligned(ptr);
}

#define XRT_MALLOC(sz) test_malloc(sz)
#define XRT_CALLOC(n, sz) test_calloc((n), (sz))
#define XRT_REALLOC(p, sz) test_realloc((p), (sz))
#define XRT_FREE(p) test_free(p)
#define XRT_ALLOC_ALIGNED(sz) test_alloc_aligned(sz)
#define XRT_FREE_ALIGNED(p) test_free_aligned(p)
#define XRT_ASSERTION_FORMAT_STEP(step) test_assertion_format_step(step)
#define XRT_IMPL

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundefined-internal"
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "../../../src/aot/xrt.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#define CHECK(condition, message)                                                                 \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            fprintf(stderr, "FAIL: %s\n", message);                                               \
            g_failed++;                                                                           \
        } else {                                                                                  \
            g_passed++;                                                                           \
        }                                                                                         \
    } while (0)

static void test_assertion_format_step(unsigned step) {
    if (g_fail_format_step == (int) step)
        xrt_throw_exc(xrt_exception_new_value(75, "formatter", 9));
}

typedef struct TestPoint {
    XrObjHeader header;
    int64_t x;
    int64_t y;
} TestPoint;

static XrValue make_point(uint16_t type_id, int64_t x, int64_t y) {
    TestPoint *point = (TestPoint *) xrt_obj_alloc(type_id, sizeof(*point));
    point->x = x;
    point->y = y;
    return xrt_box_obj(point);
}

static int64_t point_hash(struct xrt_closure *closure, void *raw) {
    (void) closure;
    const TestPoint *point = (const TestPoint *) raw;
    return point->x * 31 + point->y;
}

static uint8_t point_equal(struct xrt_closure *closure, void *left, void *right) {
    (void) closure;
    const TestPoint *a = (const TestPoint *) left;
    const TestPoint *b = (const TestPoint *) right;
    return (uint8_t) (a->x == b->x && a->y == b->y);
}

static XrValue action_normal_string(xrt_closure_t *closure) {
    (void) closure;
    return xr_box_str("owned normal action result");
}

static XrValue action_typed_error(xrt_closure_t *closure) {
    (void) closure;
    xrt_pending_error = xrt_exception_new_value(71, "typed", 5);
    return xr_box_str("owned typed action result");
}

static XrValue action_panic(xrt_closure_t *closure) {
    (void) closure;
    xrt_throw_exc(xrt_exception_new_value(72, "panic", 5));
}

static XrValue action_conflict(xrt_closure_t *closure) {
    (void) closure;
    xrt_pending_error = xrt_exception_new_value(73, "typed", 5);
    xrt_throw_exc(xrt_exception_new_value(74, "panic", 5));
}

typedef XrValue (*TestActionEntry)(xrt_closure_t *);

typedef struct TestActionDescriptor {
    TestActionEntry entry;
    XrAotCallableDesc callable;
} TestActionDescriptor;

static XrValue make_action(XrValue (*entry)(xrt_closure_t *)) {
    static const TestActionDescriptor descriptors[] = {
        {action_normal_string, {1, 0, 0, (void (*)(void)) action_normal_string}},
        {action_typed_error, {2, 0, 0, (void (*)(void)) action_typed_error}},
        {action_panic, {3, 0, 0, (void (*)(void)) action_panic}},
        {action_conflict, {4, 0, 0, (void (*)(void)) action_conflict}},
    };
    for (size_t i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]); i++) {
        if (descriptors[i].entry == entry)
            return xrt_closure_new(&descriptors[i].callable, 0);
    }
    fprintf(stderr, "make_action: unregistered test action\n");
    abort();
}

static XrValue catch_assertion_action(XrValue action,
                                      XrCoreIntrinsicExpectedFailureChannel expected) {
    XrtExcFrame frame;
    frame.prev = xrt_exc_top;
    frame.exception = XR_NULL_VAL;
    xrt_exc_top = &frame;
    if (setjmp(frame.buf) == 0) {
        (void) xrt_assertion_action(action, expected, "ownership.xr", 9, 3, 9, 20,
                                    XR_NULL_VAL);
        xrt_exc_top = frame.prev;
        return XR_NULL_VAL;
    }
    xrt_exc_top = frame.prev;
    return frame.exception;
}

static XrValue make_i64_array(int64_t a, int64_t b) {
    XrValue value = xrt_array_new(2);
    xrt_array_push(value, XR_FROM_INT(a));
    xrt_array_push(value, XR_FROM_INT(b));
    return value;
}

static void test_scalar_and_nominal_matrix(void) {
    CHECK(xrt_assertion_deep_equal(XR_FROM_FLOAT(0.0), XR_FROM_FLOAT(-0.0)),
          "signed zero compares equal");
    CHECK(!xrt_assertion_deep_equal(XR_FROM_FLOAT(NAN), XR_FROM_FLOAT(NAN)),
          "NaN follows ordinary language equality");
    CHECK(!xrt_assertion_deep_equal(XR_FROM_INT(0), XR_FROM_FLOAT(0.0)),
          "deep equality is tag-strict");

    static const XrtInspectField fields[] = {
        {"x", (uint16_t) offsetof(TestPoint, x), XR_NATIVE_I64, 0, 0, NULL},
        {"y", (uint16_t) offsetof(TestPoint, y), XR_NATIVE_I64, 0, 0, NULL},
    };
    uint16_t point_type = xrt_type_register_hot(0, NULL, 0, NULL, NULL, sizeof(TestPoint));
    uint16_t other_type = xrt_type_register_hot(0, NULL, 0, NULL, NULL, sizeof(TestPoint));
    uint16_t identity_type = xrt_type_register_hot(0, NULL, 0, NULL, NULL, sizeof(TestPoint));
    xrt_type_set_derive(point_type, XR_DERIVE_EQ, fields, 2);
    xrt_type_set_derive(other_type, XR_DERIVE_EQ, fields, 2);
    xrt_type_set_user_hash_eq(point_type, point_hash, point_equal);
    xrt_type_set_user_hash_eq(other_type, point_hash, point_equal);

    XrValue point_a = make_point(point_type, 1, 2);
    XrValue point_b = make_point(point_type, 1, 2);
    XrValue point_changed = make_point(point_type, 1, 3);
    XrValue wrong_nominal = make_point(other_type, 1, 2);
    XrValue identity_a = make_point(identity_type, 1, 2);
    XrValue identity_b = make_point(identity_type, 1, 2);
    CHECK(xrt_assertion_deep_equal(point_a, point_b),
          "same nominal derived instance compares fields");
    CHECK(!xrt_assertion_deep_equal(point_a, point_changed),
          "derived field mutation is observable");
    CHECK(!xrt_assertion_deep_equal(point_a, wrong_nominal),
          "different nominal types never alias by field shape");
    CHECK(!xrt_assertion_deep_equal(identity_a, identity_b),
          "non-derived instances retain identity equality");
    CHECK(xrt_assertion_deep_equal(identity_a, identity_a),
          "an identical non-derived instance is reflexive");

    XrtTypeDeriveInfo saved = xrt_type_derive_table[point_type];
    xrt_type_derive_table[point_type].inspect_fields = NULL;
    CHECK(!xrt_assertion_deep_equal(point_a, point_b),
          "missing derived field table fails closed");
    xrt_type_derive_table[point_type] = saved;
    xrt_type_derive_table[other_type].derive_flags = 0;
    CHECK(!xrt_assertion_deep_equal(point_a, wrong_nominal),
          "derive flag mutation cannot erase nominal identity");
}

static void test_enum_and_container_matrix(void) {
    XrValue payload_a[] = {XR_FROM_INT(7)};
    XrValue payload_b[] = {XR_FROM_INT(7)};
    XrValue payload_changed[] = {XR_FROM_INT(8)};
    XrValue enum_a =
        xrt_enum_aggregate_box(xrt_enum_aggregate_make(41, 2, 1, "Choice", "Value", payload_a));
    XrValue enum_b =
        xrt_enum_aggregate_box(xrt_enum_aggregate_make(41, 2, 1, "Choice", "Value", payload_b));
    XrValue enum_changed = xrt_enum_aggregate_box(
        xrt_enum_aggregate_make(41, 2, 1, "Choice", "Value", payload_changed));
    XrValue wrong_variant =
        xrt_enum_aggregate_box(xrt_enum_aggregate_make(41, 3, 1, "Choice", "Other", payload_b));
    XrValue wrong_nominal =
        xrt_enum_aggregate_box(xrt_enum_aggregate_make(42, 2, 1, "Other", "Value", payload_b));
    CHECK(xrt_assertion_deep_equal(enum_a, enum_b), "enum payload equality is structural");
    CHECK(!xrt_assertion_deep_equal(enum_a, enum_changed), "enum payload mutation is observed");
    CHECK(!xrt_assertion_deep_equal(enum_a, wrong_variant), "enum variant is part of equality");
    CHECK(!xrt_assertion_deep_equal(enum_a, wrong_nominal),
          "enum nominal layout is part of equality");

    XrValue array_a = make_i64_array(1, 2);
    XrValue array_b = make_i64_array(1, 2);
    XrValue array_changed = make_i64_array(1, 3);
    CHECK(xrt_assertion_deep_equal(array_a, array_b), "arrays recurse by element");
    CHECK(!xrt_assertion_deep_equal(array_a, array_changed),
          "array element mutation is observed");

    XrValue map_a_value = xrt_map_new(2);
    XrValue map_b_value = xrt_map_new(2);
    xrt_map_set((xrt_map_t *) map_a_value.ptr, xr_box_str("a"), make_i64_array(1, 2));
    xrt_map_set((xrt_map_t *) map_a_value.ptr, xr_box_str("b"), XR_FROM_INT(3));
    xrt_map_set((xrt_map_t *) map_b_value.ptr, xr_box_str("b"), XR_FROM_INT(3));
    xrt_map_set((xrt_map_t *) map_b_value.ptr, xr_box_str("a"), make_i64_array(1, 2));
    CHECK(xrt_assertion_deep_equal(map_a_value, map_b_value),
          "maps compare independent of insertion order");
    xrt_map_set((xrt_map_t *) map_b_value.ptr, xr_box_str("b"), XR_FROM_INT(4));
    CHECK(!xrt_assertion_deep_equal(map_a_value, map_b_value),
          "map value mutation is observed");

    XrValue set_a_value = xrt_set_new(3);
    XrValue set_b_value = xrt_set_new(3);
    xrt_set_add((xrt_set_t *) set_a_value.ptr, XR_FROM_INT(1));
    xrt_set_add((xrt_set_t *) set_a_value.ptr, XR_FROM_INT(2));
    xrt_set_add((xrt_set_t *) set_b_value.ptr, XR_FROM_INT(2));
    xrt_set_add((xrt_set_t *) set_b_value.ptr, XR_FROM_INT(1));
    CHECK(xrt_assertion_deep_equal(set_a_value, set_b_value),
          "sets compare independent of insertion order");
    xrt_set_add((xrt_set_t *) set_b_value.ptr, XR_FROM_INT(3));
    CHECK(!xrt_assertion_deep_equal(set_a_value, set_b_value),
          "set cardinality mutation is observed");
}

static void test_collection_key_equivalence_matrix(void) {
    XrValue nan_map_a = xrt_map_new(1);
    XrValue nan_map_b = xrt_map_new(1);
    xrt_map_set((xrt_map_t *) nan_map_a.ptr, XR_FROM_FLOAT(NAN), XR_FROM_INT(7));
    xrt_map_set((xrt_map_t *) nan_map_b.ptr, XR_FROM_FLOAT(nan("assertion")), XR_FROM_INT(7));
    CHECK(xrt_assertion_deep_equal(nan_map_a, nan_map_b),
          "Map key equality treats every NaN as one reflexive key");

    XrValue zero_map_a = xrt_map_new(1);
    XrValue zero_map_b = xrt_map_new(1);
    xrt_map_set((xrt_map_t *) zero_map_a.ptr, XR_FROM_FLOAT(0.0), XR_FROM_INT(8));
    xrt_map_set((xrt_map_t *) zero_map_b.ptr, XR_FROM_FLOAT(-0.0), XR_FROM_INT(8));
    CHECK(xrt_assertion_deep_equal(zero_map_a, zero_map_b),
          "Map key equality folds signed zero");

    static const XrtInspectField fields[] = {
        {"x", (uint16_t) offsetof(TestPoint, x), XR_NATIVE_I64, 0, 0, NULL},
        {"y", (uint16_t) offsetof(TestPoint, y), XR_NATIVE_I64, 0, 0, NULL},
    };
    uint16_t key_type = xrt_type_register_hot(0, NULL, 0, NULL, NULL, sizeof(TestPoint));
    uint16_t other_type = xrt_type_register_hot(0, NULL, 0, NULL, NULL, sizeof(TestPoint));
    xrt_type_set_derive(key_type, XR_DERIVE_EQ | XR_DERIVE_HASH, fields, 2);
    xrt_type_set_derive(other_type, XR_DERIVE_EQ | XR_DERIVE_HASH, fields, 2);
    xrt_type_set_user_hash_eq(key_type, point_hash, point_equal);
    xrt_type_set_user_hash_eq(other_type, point_hash, point_equal);
    XrValue point_map_a = xrt_map_new(1);
    XrValue point_map_b = xrt_map_new(1);
    XrValue point_map_wrong = xrt_map_new(1);
    xrt_map_set((xrt_map_t *) point_map_a.ptr, make_point(key_type, 1, 2), XR_FROM_INT(9));
    xrt_map_set((xrt_map_t *) point_map_b.ptr, make_point(key_type, 1, 2), XR_FROM_INT(9));
    xrt_map_set((xrt_map_t *) point_map_wrong.ptr, make_point(other_type, 1, 2), XR_FROM_INT(9));
    CHECK(xrt_assertion_deep_equal(point_map_a, point_map_b),
          "Map delegates derived structural keys to canonical key equality");
    CHECK(!xrt_assertion_deep_equal(point_map_a, point_map_wrong),
          "Map key lookup rejects equal fields from another nominal type");
}

static void test_cycle_matrix(void) {
    XrValue array_a = xrt_array_new(1);
    XrValue array_b = xrt_array_new(1);
    xrt_array_push(array_a, array_a);
    xrt_array_push(array_b, array_b);
    CHECK(xrt_assertion_deep_equal(array_a, array_b),
          "isomorphic array cycles terminate and compare equal");
    xrt_array_push(array_b, XR_FROM_INT(1));
    CHECK(!xrt_assertion_deep_equal(array_a, array_b),
          "array cycle cardinality mutation is observed");

    XrValue map_a = xrt_map_new(1);
    XrValue map_b = xrt_map_new(1);
    xrt_map_set((xrt_map_t *) map_a.ptr, xr_box_str("self"), map_a);
    xrt_map_set((xrt_map_t *) map_b.ptr, xr_box_str("self"), map_b);
    CHECK(xrt_assertion_deep_equal(map_a, map_b),
          "isomorphic map cycles terminate and compare equal");
    xrt_map_set((xrt_map_t *) map_b.ptr, xr_box_str("extra"), XR_FROM_INT(1));
    CHECK(!xrt_assertion_deep_equal(map_a, map_b),
          "map cycle cardinality mutation is observed");

    XrValue set_a = xrt_set_new(1);
    XrValue set_b = xrt_set_new(1);
    xrt_set_add((xrt_set_t *) set_a.ptr, set_a);
    xrt_set_add((xrt_set_t *) set_b.ptr, set_b);
    CHECK(xrt_assertion_deep_equal(set_a, set_a), "a cyclic set is reflexive");
    CHECK(!xrt_assertion_deep_equal(set_a, set_b),
          "distinct cyclic set keys do not invent pointer-key equivalence");

    XrValue bisim_left = xrt_array_new(1);
    XrValue bisim_right_a = xrt_array_new(1);
    XrValue bisim_right_b = xrt_array_new(1);
    xrt_array_push(bisim_left, bisim_left);
    xrt_array_push(bisim_right_a, bisim_right_b);
    xrt_array_push(bisim_right_b, bisim_right_a);
    CHECK(xrt_assertion_deep_equal(bisim_left, bisim_right_a),
          "cycle equality is coinductive over bisimilar graphs");
}

static void test_failure_and_action_ownership(void) {
    XrtExecutionArena *arena = xrt_execution_current();
    int64_t baseline = xrt_execution_arena_live_objects(arena);
    XrValue exception = xrt_assertion_failure_exception(
        XR_ASSERTION_FAILURE_VALUES_NOT_EQUAL, "ownership.xr", 3, 2, 3, 11,
        XR_NULL_VAL, XR_FROM_INT(1), XR_FROM_INT(2), XR_NULL_VAL, XR_NULL_VAL);
    CHECK(!XR_IS_NULL(exception), "non-string values render an owned assertion exception");
    xrt_release(exception);
    CHECK(xrt_execution_arena_live_objects(arena) == baseline,
          "owned value-to-string temporaries are released with the rendered exception");

    XrValue actual = xr_box_str("left");
    XrValue expected = xr_box_str("right");
    baseline = xrt_execution_arena_live_objects(arena);
    exception = xrt_assertion_failure_exception(
        XR_ASSERTION_FAILURE_VALUES_NOT_EQUAL, "ownership.xr", 4, 2, 4, 11,
        XR_NULL_VAL, actual, expected, XR_NULL_VAL, XR_NULL_VAL);
    xrt_release(exception);
    CHECK(xrt_execution_arena_live_objects(arena) == baseline,
          "borrowed string operands are neither leaked nor released by rendering");

    g_fail_next_malloc = 1;
    exception = xrt_assertion_failure_exception(
        XR_ASSERTION_FAILURE_VALUES_NOT_EQUAL, "ownership.xr", 5, 2, 5, 11,
        XR_NULL_VAL, actual, expected, XR_NULL_VAL, XR_NULL_VAL);
    CHECK(!XR_IS_NULL(exception), "render allocation failure produces a bounded exception");
    xrt_release(exception);
    CHECK(xrt_execution_arena_live_objects(arena) == baseline,
          "render allocation failure releases every temporary");
    xrt_release(actual);
    xrt_release(expected);

    for (int step = 0; step < 4; step++) {
        baseline = xrt_execution_arena_live_objects(arena);
        g_fail_format_step = step;
        exception = xrt_assertion_failure_exception(
            XR_ASSERTION_FAILURE_CONFLICTING_CHANNELS, "ownership.xr", 6, 2, 6, 11,
            XR_NULL_VAL, XR_FROM_INT(1), XR_FROM_INT(2), XR_FROM_INT(3), XR_FROM_INT(4));
        g_fail_format_step = -1;
        CHECK(!XR_IS_NULL(exception), "formatter panic becomes a bounded assertion exception");
        xrt_release(exception);
        CHECK(xrt_execution_arena_live_objects(arena) == baseline,
              "each formatter panic releases all earlier owned text");
    }

    const struct {
        XrValue (*entry)(xrt_closure_t *);
        XrCoreIntrinsicExpectedFailureChannel expected;
        bool should_throw;
        const char *label;
    } cases[] = {
        {action_typed_error, XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR, false,
         "typed-error success consumes caught error and normal result"},
        {action_panic, XR_CORE_INTRINSIC_FAILURE_CHANNEL_PANIC, false,
         "panic success consumes caught panic without reading a nonexistent return value"},
        {action_normal_string, XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR, true,
         "normal return consumes owned result before no-failure report"},
        {action_panic, XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR, true,
         "wrong panic channel releases caught panic before report"},
        {action_typed_error, XR_CORE_INTRINSIC_FAILURE_CHANNEL_PANIC, true,
         "wrong typed-error channel releases caught error before report"},
        {action_conflict, XR_CORE_INTRINSIC_FAILURE_CHANNEL_TYPED_ERROR, true,
         "conflicting channels release both observations before report"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        XrValue action = make_action(cases[i].entry);
        baseline = xrt_execution_arena_live_objects(arena);
        exception = catch_assertion_action(action, cases[i].expected);
        CHECK(cases[i].should_throw ? !XR_IS_NULL(exception) : XR_IS_NULL(exception),
              cases[i].label);
        xrt_release(exception);
        CHECK(xrt_execution_arena_live_objects(arena) == baseline,
              "action observations and failure temporaries return to their live baseline");
        xrt_release(action);
    }
    CHECK(XR_IS_NULL(xrt_pending_error), "assertion action never leaks a pending typed error");
}

int main(void) {
    XrtExecutionArena *arena = xrt_execution_arena_new();
    void *previous = xrt_execution_arena_enter(arena);
    test_scalar_and_nominal_matrix();
    test_enum_and_container_matrix();
    test_collection_key_equivalence_matrix();
    test_cycle_matrix();
    test_failure_and_action_ownership();
    xrt_execution_arena_restore(previous);
    xrt_execution_arena_destroy(arena);

    printf("test_xrt_assertion: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
