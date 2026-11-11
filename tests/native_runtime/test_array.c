/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_array.c - Canonical Array COW across root, state and context retirement
 */
#include "runtime/native/xnative_array.h"
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_module_state.h"
#include "runtime/native/xnative_string.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_array.h"
#include "xir/xxir_view_value_internal.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_XIR_VALUE_OK)
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
#include "native_array_faults.h"
#endif

static const char original[] = "Array retains a long String\0and UTF-8 \xf0\x9f\x8c\x8d";
typedef struct ArrayContext {
    XrtModuleState *state;
    XrtModuleRef *reference;
    XrXirDomain *pin;
    uint64_t baseline;
    uint32_t phase;
    bool string, completed, immutable;
} ArrayContext;

static void string_equals(XrtString *string, const char *bytes, size_t length) {
    size_t actual = 0;
    const char *data = xrt_string_bytes(string, &actual);
    CHECK(actual == length && !memcmp(data, bytes, length));
}
static XrtArray *state_array(ArrayContext *context) {
    const void *payload = NULL;
    OK(xrt_module_borrow(context->reference, &payload));
    XrtArray *array;
    memcpy(&array, payload, sizeof(array));
    return array;
}
static void state_new(ArrayContext *context) {
    XrtModuleTypeSpec type = {context->string ? XRT_MODULE_ARRAY_STRING : XRT_MODULE_ARRAY_I64};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slot = {.flags=context->immutable ? 0 : XRT_MODULE_SLOT_MUTABLE, .size=8, .alignment=8};
    XrtModuleLayoutSpec spec = {.types=&type, .type_count=1, .modules=&module, .module_count=1,
        .slots=&slot, .slot_count=1, .payload_size=8, .payload_alignment=8, .byte_limit=UINT64_MAX};
    XrtModuleLayout *layout = NULL;
    OK(xrt_module_layout_new(&spec, &layout));
    OK(xrt_module_state_new(layout, UINT64_MAX, &context->state));
    xrt_module_layout_drop(&layout);
}
static void initialize_root(void *opaque) {
    ArrayContext *context = opaque;
    OK(xrt_module_begin(context->state, 0));
    XrtValueRoot *root = xrt_value_root_current(1);
    context->pin = xrt_value_root_domain(root, 1);
    CHECK(xr_xir_domain_retain(context->pin));
    context->baseline = xr_xir_domain_stats(context->pin).live_bytes;
    XrtArray *array;
    if (context->string) {
        XrtString *string = xrt_string_literal(original, sizeof(original) - 1, 1);
        XrtString *values[] = {string, string};
        array = xrt_array_string_new(values, 2, 1);
    } else {
        int64_t values[] = {11, 22};
        array = xrt_array_i64_new(values, 2, 1);
    }
    OK(xrt_module_construct(context->state, 0, &array, &context->reference));
    OK(xrt_module_publish(context->reference));
    OK(xrt_module_ready(context->state, 0));
    XrtModuleRef *published = NULL;
    OK(xrt_module_reference(context->state, 0, &published));
    context->reference = published;
    XrtArray *before = state_array(context), *wrong;
    if (context->string) {
        int64_t value = 0;
        wrong = xrt_array_i64_new(&value, 1, 1);
    } else {
        XrtString *value = xrt_string_literal("wrong element", 13, 1);
        wrong = xrt_array_string_new(&value, 1, 1);
    }
    CHECK(xrt_module_replace(context->reference, &wrong) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(state_array(context) == before);
    xrt_array_drop(&wrong);
    if (context->string) {
        XrtString *next = xrt_string_literal("changed", 7, 1);
        xrt_array_string_set(&array, 0, next, 1);
        XrtString *saved = xrt_array_string_get(state_array(context), 0, 1);
        string_equals(saved, original, sizeof(original) - 1);
        xrt_string_drop(&saved);
    } else {
        xrt_array_i64_set(&array, 0, 99, 1);
        CHECK(xrt_array_i64_get(state_array(context), 0, 1) == 11);
    }
    xrt_array_drop(&array);
}
static void state_retires_first(void *opaque) {
    ArrayContext *context = opaque;
    context->phase = 1;
    XrtArray *array = xrt_array_retain(state_array(context), 1);
    context->phase = 2;
    xrt_module_state_drop(&context->state);
    context->phase = 3;
    CHECK(xrt_array_length(array, 1) == 2);
    context->phase = 4;
    if (context->string) {
        XrtString *string = xrt_array_string_get(array, 0, 1);
        string_equals(string, original, sizeof(original) - 1);
        xrt_array_string_push(&array, string, 1);
        xrt_string_drop(&string);
    } else {
        CHECK(xrt_array_i64_get(array, 0, 1) == 11);
        context->phase = 5;
        xrt_array_i64_push(&array, 33, 1);
    }
    CHECK(xrt_array_length(array, 1) == 3);
    context->completed = true;
    /* The common root owns this final Array even without a normal drop. */
}
static void cow_and_rebind(void *opaque) {
    bool *completed = opaque;
    int64_t first[] = {1}, replacement[] = {7};
    XrtArray *cell = xrt_array_i64_new(first, 1, 1);
    XrtArray *saved = xrt_array_retain(cell, 1);
    CHECK(atomic_load(&((XirObject *)cell)->references) == 2);
    XrtArray *next = xrt_array_i64_new(replacement, 1, 1);
    /* An evaluated RHS replaces the root before set binds its current value. */
    xrt_array_assign(&cell, next, 1);
    xrt_array_i64_set(&cell, 0, 99, 1);
    CHECK(xrt_array_i64_get(saved, 0, 1) == 1);
    CHECK(xrt_array_i64_get(next, 0, 1) == 7);
    CHECK(xrt_array_i64_get(cell, 0, 1) == 99);
    xrt_array_assign(&cell, cell, 1);
    xrt_array_i64_push(&cell, 100, 1);
    CHECK(xrt_array_i64_get(cell, 1, 1) == 100);
    xrt_array_drop(&saved); xrt_array_drop(&next); xrt_array_drop(&cell);
    *completed = true;
}
static void immutable_root(void *opaque) {
    ArrayContext *context = opaque;
    OK(xrt_module_begin(context->state, 0));
    context->pin = xrt_value_root_domain(xrt_value_root_current(1), 1);
    CHECK(xr_xir_domain_retain(context->pin));
    context->baseline = xr_xir_domain_stats(context->pin).live_bytes;
    int64_t value = 11;
    XrtArray *array = xrt_array_i64_new(&value, 1, 1);
    OK(xrt_module_initialize(context->state, 0, &array));
    OK(xrt_module_ready(context->state, 0));
    OK(xrt_module_reference(context->state, 0, &context->reference));
    CHECK(xrt_module_replace(context->reference, &array) == XR_XIR_VALUE_BAD_ARGUMENT);
    xrt_array_i64_set(&array, 0, 99, 1);
    CHECK(xrt_array_i64_get(state_array(context), 0, 1) == 11);
    xrt_array_drop(&array);
}
static void oversized_root(void *opaque) {
    (void)opaque;
    int64_t value = 1;
    (void)xrt_array_i64_new(&value, SIZE_MAX, 1);
}
static XrtLocationOwner *locations(void) {
    XrtSourceLocation source = {"array.xr", 8, 3, 1, 3, 9};
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(&source, 1, &owner) == XRT_LOCATION_OK);
    return owner;
}
static void retirement_cases(XrtLocationOwner *owner, bool string) {
    ArrayContext context = {.string=string};
    state_new(&context);
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(owner, &failure, initialize_root, &context) == XRT_EXECUTION_COMPLETED);
    CHECK(xr_xir_domain_stats(context.pin).live_bytes > context.baseline);
    XrtExecutionStatus status = xrt_run_embedded(owner, &failure, state_retires_first, &context);
    if (status != XRT_EXECUTION_COMPLETED)
        fprintf(stderr, "retirement string=%u phase=%u kind=%u value=%u source=%u\n", (unsigned)string,
            context.phase, (unsigned)failure.kind, (unsigned)failure.detail.value.status, failure.detail.value.source_id);
    CHECK(status == XRT_EXECUTION_COMPLETED);
    CHECK(context.completed && !context.state);
    XrXirDomainStats stats = xr_xir_domain_stats(context.pin);
    CHECK(stats.live_bytes == context.baseline && stats.allocations == stats.frees + 1);
    xr_xir_domain_drop(context.pin);
}

static void cross_context(void) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before = {0}, after = {0};
    CHECK(xr_compile_resources_stats(context.resources, &before) == XR_COMPILE_RESOURCE_OK);
    XrXirTypeNode nodes[] = {{.kind=XR_XIR_TYPE_ARRAY, .element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_CELL, .element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_SLICE, .element=XR_XIR_STRING}};
    XrXirTypes types = {nodes, 3, NULL, NULL};
    XrXirTypeArena *arena = NULL, *receiver = NULL;
    OK(xr_xir_compile_type_arena_new(&context, &types, &arena));
    uint64_t bytes = 0;
    OK(xrt_array_types_new(UINT64_MAX, &receiver, &bytes));
    XrXirDomain *domain = NULL;
    OK(xr_xir_domain_new(UINT64_MAX, &domain));
    uint64_t baseline = xr_xir_domain_stats(domain).live_bytes;
    XrXirValue string = {0}, value = {0};
    OK(xr_xir_string_new(domain, original, sizeof(original) - 1, &string));
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, UINT64_MAX, UINT64_MAX};
    OK(xr_xir_array_new((XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE, &string, 1, &admission, &value));
    XrtArray *source = (XrtArray *)(intptr_t)value.payload, *copy = NULL;
    CHECK(xrt_array_owned_copy((XrtArray *)(intptr_t)string.payload, receiver,
        XRT_ARRAY_STRING, &copy) == XR_XIR_VALUE_BAD_ARGUMENT && !copy);
    CHECK(xrt_array_owned_copy(source, receiver, XRT_ARRAY_I64, &copy) == XR_XIR_VALUE_BAD_ARGUMENT && !copy);
    XrXirValue cell = {0}, view = {0};
    OK(xr_xir_cell_new(domain, arena, (XrXirType)257, &value, &admission, &cell));
    XrXirViewStorageSpec spec = {{&context, &cell, 1}, 0, 1, XR_XIR_TYPE_SLICE};
    OK(xr_xir_view_new((XrXirType)258, &cell, NULL, &spec, &admission, &view));
    CHECK(xrt_array_owned_copy((XrtArray *)(intptr_t)cell.payload, receiver,
        XRT_ARRAY_STRING, &copy) == XR_XIR_VALUE_BAD_ARGUMENT && !copy);
    CHECK(xrt_array_owned_copy((XrtArray *)(intptr_t)view.payload, receiver,
        XRT_ARRAY_STRING, &copy) == XR_XIR_VALUE_BAD_ARGUMENT && !copy);
    XrXirViewOriginPin pin = {0};
    xr_xir_view_scope_pin_created(&view, &spec.authority, &pin);
    xr_xir_view_scope_close(&pin);
    xr_xir_value_drop(&view); xr_xir_value_drop(&cell);
    OK(xrt_array_owned_copy(source, receiver, XRT_ARRAY_STRING, &copy));
    xr_xir_value_drop(&string); xr_xir_value_drop(&value);
    xr_xir_compile_type_arena_drop(arena); xr_xir_compile_type_arena_drop(receiver);
    CHECK(xr_xir_domain_stats(domain).live_bytes > baseline);
    xrt_array_owned_drop(&copy);
    CHECK(xr_xir_domain_stats(domain).live_bytes == baseline);
    CHECK(xr_compile_resources_stats(context.resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.held_bytes == before.held_bytes);
    xr_xir_domain_drop(domain); xr_compile_resources_release(context.resources);
}

static void failing_read(void *opaque) {
    ArrayContext *context = opaque;
    XrtString *string = xrt_array_string_get(state_array(context), 0, 1);
    string_equals(string, original, sizeof(original) - 1);
    xrt_string_drop(&string);
}
static void failing_mutation(void *opaque) {
    ArrayContext *context = opaque;
    XrtArray *array = xrt_array_retain(state_array(context), 1);
    xrt_array_i64_set(&array, 0, 77, 1);
    for (int64_t n = 0; n < 8; ++n) xrt_array_i64_push(&array, n, 1);
    xrt_array_drop(&array);
}

typedef struct ArrayCacheProbe {
    uint32_t operation;
    XrXirDomain *pin;
    uint64_t baseline;
} ArrayCacheProbe;

static void cached_shape_failure(void *opaque) {
    ArrayCacheProbe *probe = opaque;
    XrtValueRoot *root = xrt_value_root_current(1);
    probe->pin = xrt_value_root_domain(root, 1);
    CHECK(xr_xir_domain_retain(probe->pin));
    probe->baseline = xr_xir_domain_stats(probe->pin).live_bytes;
    int64_t value = 17;
    XrtString *text = xrt_string_literal("cache", 5, 1);
    XrtArray *integers = xrt_array_i64_new(&value, 1, 1);
    XrtArray *strings = xrt_array_string_new(&text, 1, 1);
    for (uint32_t i = 0; i < 40; ++i) {
        if (i & 1) (void)xrt_array_i64_new(&value, 1, 1);
        else (void)xrt_array_string_new(&text, 1, 1);
    }
    CHECK(xrt_array_length(integers, 1) == 1 && xrt_array_length(strings, 1) == 1);
    CHECK(xrt_array_i64_get(integers, 0, 1) == value);
    if (probe->operation == 0) (void)xrt_array_string_get(integers, 0, 1);
    else if (probe->operation == 1) (void)xrt_array_i64_get(strings, 0, 1);
    else if (probe->operation == 2) xrt_array_i64_push(&strings, value, 1);
    else if (probe->operation == 3) xrt_array_string_push(&integers, text, 1);
    else (void)xrt_array_i64_get(integers, probe->operation == 4 ? -1 : 1, 1);
    CHECK(false);
}

static void cached_shape_cases(XrtLocationOwner *locations) {
    for (uint32_t operation = 0; operation < 6; ++operation) {
        ArrayCacheProbe probe = {.operation=operation};
        XrtExecutionFailure failure = {0};
        XrtValueRoot root = {0};
        CHECK(xrt_run_embedded_owned(locations, &failure, cached_shape_failure, &probe, &root) == XRT_EXECUTION_FAILURE);
        CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status ==
            (operation < 4 ? XR_XIR_VALUE_BAD_ARGUMENT : XR_XIR_VALUE_BOUNDS) &&
            failure.detail.value.source_id == 1 && xrt_value_root_empty(&root));
        CHECK(xr_xir_domain_stats(probe.pin).live_bytes == probe.baseline);
        xr_xir_domain_drop(probe.pin);
    }
}
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
static void state_allocation_sweep(void) {
    XrtModuleTypeSpec type = {XRT_MODULE_ARRAY_STRING};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slot = {.flags=XRT_MODULE_SLOT_MUTABLE, .size=8, .alignment=8};
    XrtModuleLayoutSpec spec = {.types=&type, .type_count=1, .modules=&module, .module_count=1,
        .slots=&slot, .slot_count=1, .payload_size=8, .payload_alignment=8, .byte_limit=UINT64_MAX};
    XrtModuleLayout *layout = NULL;
    native_array_calls = 0;
    OK(xrt_module_layout_new(&spec, &layout));
    size_t layout_points = native_array_calls;
    xrt_module_layout_drop(&layout);
    CHECK(!native_array_live && !native_array_bytes);
    for (size_t point = 0; point < layout_points; ++point) {
        native_array_calls = 0; native_array_fail_at = point; native_array_injected = false;
        CHECK(xrt_module_layout_new(&spec, &layout) == XR_XIR_VALUE_OOM && !layout);
        CHECK(native_array_injected && !native_array_live && !native_array_bytes);
    }
    native_array_fail_at = SIZE_MAX;
    OK(xrt_module_layout_new(&spec, &layout));
    size_t live = native_array_live, bytes = native_array_bytes;
    XrtModuleState *state = NULL;
    native_array_calls = 0;
    OK(xrt_module_state_new(layout, UINT64_MAX, &state));
    size_t state_points = native_array_calls;
    xrt_module_state_drop(&state);
    CHECK(native_array_live == live && native_array_bytes == bytes);
    for (size_t point = 0; point < state_points; ++point) {
        native_array_calls = 0; native_array_fail_at = point; native_array_injected = false;
        CHECK(xrt_module_state_new(layout, UINT64_MAX, &state) == XR_XIR_VALUE_OOM && !state);
        CHECK(native_array_injected && native_array_live == live && native_array_bytes == bytes);
    }
    native_array_fail_at = SIZE_MAX;
    xrt_module_layout_drop(&layout);
    CHECK(!native_array_live && !native_array_bytes);
    printf("Array layout/state allocation failure points: %zu/%zu\n", layout_points, state_points);
}

static void allocation_sweep(XrtLocationOwner *owner, bool string) {
    ArrayContext context = {.string=string};
    state_new(&context);
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(owner, &failure, initialize_root, &context) == XRT_EXECUTION_COMPLETED);
    size_t baseline = native_array_live, bytes = native_array_bytes;
    native_array_calls = 0;
    XrtSynchronousRoot root = string ? failing_read : failing_mutation;
    CHECK(xrt_run_embedded(owner, &failure, root, &context) == XRT_EXECUTION_COMPLETED);
    size_t points = native_array_calls;
    CHECK(native_array_live == baseline && native_array_bytes == bytes);
    bool publication_failure = false;
    for (size_t point = 0; point < points; ++point) {
        native_array_calls = 0; native_array_fail_at = point;
        native_array_injected = native_array_pending_failure = false;
        XrtValueRoot values = {0};
        CHECK(xrt_run_embedded_owned(owner, &failure, root, &context, &values) == XRT_EXECUTION_FAILURE);
        CHECK(native_array_injected && failure.kind == XRT_FAILURE_VALUE &&
            failure.detail.value.status == XR_XIR_VALUE_OOM);
        CHECK(xrt_value_root_empty(&values));
        CHECK(native_array_live == baseline && native_array_bytes == bytes);
        publication_failure |= native_array_pending_failure;
    }
    native_array_fail_at = SIZE_MAX;
    if (string) CHECK(publication_failure);
    xrt_module_state_drop(&context.state);
    CHECK(xr_xir_domain_stats(context.pin).live_bytes == context.baseline);
    xr_xir_domain_drop(context.pin);
    CHECK(!native_array_live && !native_array_bytes);
    printf("Array %s allocation failure points: %zu\n", string ? "publication" : "COW/grow", points);
}
#endif

#include "native_array_repeat_cases.h"

int main(void) {
    XrtLocationOwner *owner = locations();
    repeat_cases(owner);
    repeat_cross_root(owner);
    cached_shape_cases(owner);
    bool completed = false;
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(owner, &failure, cow_and_rebind, &completed) == XRT_EXECUTION_COMPLETED && completed);
    retirement_cases(owner, false); retirement_cases(owner, true);
    ArrayContext immutable = {.immutable=true};
    state_new(&immutable);
    CHECK(xrt_run_embedded(owner, &failure, immutable_root, &immutable) == XRT_EXECUTION_COMPLETED);
    xrt_module_state_drop(&immutable.state);
    CHECK(xr_xir_domain_stats(immutable.pin).live_bytes == immutable.baseline);
    xr_xir_domain_drop(immutable.pin);
    CHECK(xrt_run_embedded(owner, &failure, oversized_root, NULL) == XRT_EXECUTION_FAILURE);
    CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_LIMIT);
    cross_context();
    ArrayContext context = {.string=true};
    state_new(&context);
    CHECK(xrt_run_embedded(owner, &failure, initialize_root, &context) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_run_embedded(owner, &failure, failing_read, &context) == XRT_EXECUTION_COMPLETED);
    xrt_module_state_drop(&context.state); xr_xir_domain_drop(context.pin);
    context = (ArrayContext){0}; state_new(&context);
    CHECK(xrt_run_embedded(owner, &failure, initialize_root, &context) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_run_embedded(owner, &failure, failing_mutation, &context) == XRT_EXECUTION_COMPLETED);
    xrt_module_state_drop(&context.state); xr_xir_domain_drop(context.pin);
#if defined(XR_NATIVE_ARRAY_ALLOCATIONS)
    CHECK(!native_array_live && !native_array_bytes);
    allocation_sweep(owner, false); allocation_sweep(owner, true);
    state_allocation_sweep();
    repeat_allocation_sweep(owner, false);
    repeat_allocation_sweep(owner, true);
#endif
    xrt_locations_release(owner);
    puts("Canonical Array COW, state/root retirement and structural admission passed");
    return 0;
}
