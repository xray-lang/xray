/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_boxed.c - Canonical record owners survive producer and State retirement
 */
#include "runtime/native/xnative_boxed.h"
#include "runtime/native/xnative_callable.h"
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_module_state.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_types.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_XIR_VALUE_OK)
#if defined(XR_NATIVE_BOXED_ALLOCATIONS)
#include "native_array_faults.h"
#endif

static const XrtCallableParameterSpec record_fields[] = {
    {.value={XRT_CALLABLE_STRING,0}, .name="name", .name_length=4},
    {.value={XRT_CALLABLE_SCALAR,XR_POD_I64}, .name="score", .name_length=5}};
static const XrtCallableParameterSpec nested_fields[] = {
    {.value={XRT_CALLABLE_CONSTRUCTED,1}, .name="child", .name_length=5},
    {.value={XRT_CALLABLE_CONSTRUCTED,0}, .name="maybe", .name_length=5}};
static const XrtCallableTypeSpec specs[] = {
    {.kind=XRT_CALLABLE_NULLABLE, .element={XRT_CALLABLE_STRING,0}},
    {.kind=XRT_CALLABLE_RECORD, .parameters=record_fields, .parameter_count=2},
    {.kind=XRT_CALLABLE_RECORD, .parameters=nested_fields, .parameter_count=2}};
static const XrtBoxedImage image = {specs, 3};
static const char text[] = "record owns UTF-8 \xf0\x9f\x8c\x8d and an embedded\0byte";

static void string_equals(XrtString *value) {
    size_t length = 0;
    const char *bytes = xrt_string_bytes(value, &length);
    CHECK(length == sizeof(text) - 1 && !memcmp(bytes, text, length));
}
static XrtBoxed *record_new(void) {
    XrtString *name = xrt_string_new(text, sizeof(text) - 1, 0);
    uint64_t fields[] = {(uint64_t)(uintptr_t)name, 95};
    XrtBoxed *record = xrt_boxed_new(&image, 1, fields, 2, 0);
    xrt_string_drop(&name);
    return record;
}
static XrtBoxed *nested_new(bool some) {
    XrtBoxed *record = record_new();
    XrtString *name = some ? xrt_string_new(text, sizeof(text) - 1, 0) : NULL;
    uint64_t payload = (uint64_t)(uintptr_t)name;
    XrtBoxed *optional = xrt_boxed_new(&image, 0, some ? &payload : NULL, some ? 1 : 0, 0);
    if (name) xrt_string_drop(&name);
    CHECK(xrt_boxed_present(optional, 0) == some);
    if (some) {
        name = (XrtString *)(uintptr_t)xrt_boxed_field(optional, 0, 0);
        string_equals(name);
        xrt_string_drop(&name);
    }
    uint64_t fields[] = {(uint64_t)(uintptr_t)record, (uint64_t)(uintptr_t)optional};
    XrtBoxed *nested = xrt_boxed_new(&image, 2, fields, 2, 0);
    xrt_boxed_drop(record, 0);
    xrt_boxed_drop(optional, 0);
    return nested;
}
typedef struct BoxedContext {
    XrtModuleState *state;
    XrtModuleRef *reference;
    XrXirDomain *pin;
    uint64_t baseline;
    uint32_t scenario;
} BoxedContext;

static void state_new(BoxedContext *context) {
    char name[] = "name";
    XrtCallableParameterSpec fields[2];
    memcpy(fields, record_fields, sizeof(fields));
    fields[0].name = name;
    XrtCallableTypeSpec types[3];
    memcpy(types, specs, sizeof(types));
    types[1].parameters = fields;
    XrtModuleTypeSpec type = {XRT_MODULE_BOXED, 2};
    XrtModuleSpec module = {0};
    XrtModuleSlotSpec slot = {.flags=XRT_MODULE_SLOT_MUTABLE, .size=8, .alignment=8};
    XrtModuleLayoutSpec spec = {.types=&type, .type_count=1, .modules=&module, .module_count=1,
        .slots=&slot, .slot_count=1, .payload_size=8, .payload_alignment=8, .byte_limit=UINT64_MAX,
        .callable_types=types, .callable_type_count=3};
    XrtModuleLayout *layout = NULL;
    OK(xrt_module_layout_new(&spec, &layout));
    memset(name, 'x', sizeof(name));
    memset(fields, 0xa5, sizeof(fields));
    memset(types, 0xa5, sizeof(types));
    XrtModuleLayoutSpec received;
    OK(xrt_module_layout_view(layout, &received));
    CHECK(received.callable_types[1].parameters[0].name_length == 4 &&
        !memcmp(received.callable_types[1].parameters[0].name, "name", 4));
    OK(xrt_module_state_new(layout, UINT64_MAX, &context->state));
    xrt_module_layout_drop(&layout);
}
static XrtBoxed *state_value(BoxedContext *context) {
    const void *payload = NULL;
    OK(xrt_module_borrow(context->reference, &payload));
    XrtBoxed *value;
    memcpy(&value, payload, sizeof(value));
    return value;
}
static void initialize(void *opaque) {
    BoxedContext *context = opaque;
    XrtValueRoot *root = xrt_value_root_current(0);
    context->pin = xrt_value_root_domain(root, 0);
    CHECK(xr_xir_domain_retain(context->pin));
    context->baseline = xr_xir_domain_stats(context->pin).live_bytes;
    xrt_boxed_state_prepare(context->state, &image, 0);
    OK(xrt_module_begin(context->state, 0));
    XrtBoxed *nested = nested_new(false);
    OK(xrt_module_construct(context->state, 0, &nested, &context->reference));
    OK(xrt_module_publish(context->reference));
    OK(xrt_module_ready(context->state, 0));
    XrtModuleRef *published = NULL;
    OK(xrt_module_reference(context->state, 0, &published));
    context->reference = published;
    xrt_boxed_drop(nested, 0);
}
static void consume(void *opaque) {
    BoxedContext *context = opaque;
    xrt_boxed_state_prepare(context->state, &image, 0);
    XrtBoxed *before = state_value(context);
    XrtBoxed *copy = xrt_boxed_retain(before, 0);
    XrtBoxed *wrong = xrt_boxed_new(&image, 0, NULL, 0, 0);
    CHECK(xrt_module_replace(context->reference, &wrong) == XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(state_value(context) == before);
    xrt_boxed_drop(wrong, 0);
    XrtBoxed *next = nested_new(true);
    OK(xrt_module_replace(context->reference, &next));
    xrt_boxed_drop(next, 0);
    XrtBoxed *old_optional = (XrtBoxed *)(uintptr_t)xrt_boxed_field(copy, 1, 0);
    CHECK(!xrt_boxed_present(old_optional, 0));
    xrt_boxed_drop(old_optional, 0);
    XrtBoxed *child = (XrtBoxed *)(uintptr_t)xrt_boxed_field(copy, 0, 0);
    xrt_boxed_drop(copy, 0);
    xrt_module_state_drop(&context->state);
    XrtString *name = (XrtString *)(uintptr_t)xrt_boxed_field(child, 0, 0);
    string_equals(name);
    CHECK(xrt_boxed_field(child, 1, 0) == 95);
    xrt_boxed_drop(child, 0);
    string_equals(name);
    xrt_string_drop(&name);
}
static void create_values(void *opaque) {
    (void)opaque;
    XrtBoxed *a = nested_new(false), *b = nested_new(true);
    XrtBoxed *copy = xrt_boxed_retain(b, 0);
    xrt_boxed_drop(a, 0); xrt_boxed_drop(b, 0); xrt_boxed_drop(copy, 0);
}
static void invalid_handle(void *opaque) {
    BoxedContext *context = opaque;
    XirObject fake = {.type=context->scenario == 1 ? XR_XIR_F64 : XR_XIR_I64,
        .arena=(XrXirTypeArena *)(uintptr_t)1};
    XrtBoxed *box = context->scenario == 2 ? NULL : (XrtBoxed *)&fake;
    if (context->scenario < 3) (void)xrt_boxed_retain(box, 0);
    else if (context->scenario == 3) (void)xrt_boxed_present(box, 0);
    else (void)xrt_boxed_field(box, 0, 0);
    CHECK(false);
}
static void closed_shapes(void) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrXirCompileContext context = {.limits=xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrXirNominalIdentity identity = {.module={"test",4}, .name={"Plain",5}, .exported=1,
        .kind=XR_XIR_NOMINAL_STRUCT};
    XrXirNominalTable table = {.identities=&identity, .count=1};
    XrXirTypeNode nodes[] = {{.kind=XR_XIR_TYPE_NOMINAL, .nominal={0}},
        {.kind=XR_XIR_TYPE_NULLABLE, .element=XR_XIR_CONSTRUCTED_TYPE_BASE}};
    XrXirTypes types = {nodes, 2, &table, NULL};
    XrXirTypeArena *arena = NULL;
    OK(xr_xir_compile_type_arena_new(&context, &types, &arena));
    CHECK(!xrt_boxed_type(arena, (XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE));
    CHECK(!xrt_boxed_type(arena, (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + 1)));
    xr_xir_compile_type_arena_drop(arena);
    xr_compile_resources_release(context.resources);
}
#if defined(XR_NATIVE_BOXED_ALLOCATIONS)
static void allocation_sweep(XrtLocationOwner *locations) {
    XrtExecutionFailure failure = {0};
    native_array_calls = 0;
    CHECK(xrt_run_embedded(locations, &failure, create_values, NULL) == XRT_EXECUTION_COMPLETED);
    size_t points = native_array_calls;
    CHECK(!native_array_live && !native_array_bytes);
    bool publication_failure = false;
    for (size_t point = 0; point < points; ++point) {
        native_array_calls = 0; native_array_fail_at = point;
        native_array_injected = native_boxed_pending_failure = false;
        XrtValueRoot root = {0};
        CHECK(xrt_run_embedded_owned(locations, &failure, create_values, NULL, &root) == XRT_EXECUTION_FAILURE);
        CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_OOM &&
            native_array_injected && xrt_value_root_empty(&root));
        CHECK(!native_array_live && !native_array_bytes);
        publication_failure |= native_boxed_pending_failure;
    }
    native_array_fail_at = SIZE_MAX;
    CHECK(publication_failure);
    printf("Record/Nullable allocation failure points: %zu\n", points);
}
#endif
int main(void) {
    XrtLocationOwner *locations = NULL;
    CHECK(xrt_locations_create(NULL, 0, &locations) == XRT_LOCATION_OK);
    closed_shapes();
    for (uint32_t scenario = 0; scenario < 5; ++scenario) {
        BoxedContext context = {.scenario=scenario};
        XrtValueRoot root = {0};
        XrtExecutionFailure failure = {0};
        CHECK(xrt_run_embedded_owned(locations, &failure, invalid_handle, &context, &root) == XRT_EXECUTION_FAILURE);
        CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_BAD_ARGUMENT &&
            xrt_value_root_empty(&root));
    }
    BoxedContext context = {0};
    state_new(&context);
    XrtExecutionFailure failure = {0};
    CHECK(xrt_run_embedded(locations, &failure, initialize, &context) == XRT_EXECUTION_COMPLETED);
    CHECK(xrt_run_embedded(locations, &failure, consume, &context) == XRT_EXECUTION_COMPLETED);
    CHECK(!context.state && xr_xir_domain_stats(context.pin).live_bytes == context.baseline);
    xr_xir_domain_drop(context.pin);
    CHECK(xrt_run_embedded(locations, &failure, create_values, NULL) == XRT_EXECUTION_COMPLETED);
#if defined(XR_NATIVE_BOXED_ALLOCATIONS)
    allocation_sweep(locations);
#endif
    xrt_locations_release(locations);
    puts("Canonical record owners passed");
    return 0;
}
