/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_c_faults.c - Unpublished failures and the real C output ICE boundary
 */
#include "lir/xlir_builder.h"
#include "lir/xlir_c.h"
#include "lir/xlir_map.h"
#include "lir/xi_cgen_verify_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "os/os_time.h"
#include "../unit/xir/xir_sdk_resource_test.h"
static bool clock_override, clock_at_publication;
static uint64_t clock_now, publication_clock;
static uint64_t request_clock(void) {
    return clock_override ? clock_now : xr_time_monotonic_ns();
}
#define xr_time_monotonic_ns request_clock
#include "base/xcompile_request.c"
#undef xr_time_monotonic_ns
static bool fail_workspace, fail_text, fail_divisors, corrupt_text, injected;
static unsigned divisor_attempt, failed_divisor_attempt;
static bool *cancel_at_publication;
typedef struct ArrayLocalIndexCancelPoint {
    XrCompileResources *resources;
    size_t *index;
    size_t count, populated, attempts, live, bytes;
    uint64_t held;
    bool reached;
} ArrayLocalIndexCancelPoint;
static ArrayLocalIndexCancelPoint *array_local_index_watch;
static XrCompileResourceStatus workspace_allocate(XrCompileResources *r, size_t n, void **out) {
    if (fail_workspace && !injected) {
        injected = true;
        return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    }
    return xr_compile_resources_alloc(r, n, out);
}
static XrCompileResourceStatus facts_allocate(XrCompileResources *r, size_t n, size_t size, void **out) {
    /* The closure seen set is first; the new per-function facts are next. */
    if (fail_divisors && size == 1 && n == 4 &&
        ++divisor_attempt == failed_divisor_attempt) {
        injected = true;
        return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    }
    ArrayLocalIndexCancelPoint *point = array_local_index_watch;
    if (point) {
        /* This watch is armed only around the private cold-spec emitter,
         * whose sole calloc is its actual temporary coordinate index. */
        CHECK(r == point->resources && !point->index && size == sizeof(size_t));
        point->held = sdk_stats(r).held_bytes; point->live = runtime_live; point->bytes = runtime_bytes;
    }
    XrCompileResourceStatus status = xr_compile_resources_calloc(r, n, size, out);
    if (point) {
        CHECK(status == XR_COMPILE_RESOURCE_OK && *out);
        point->index = *out; point->count = n;
    }
    return status;
}
static XrCompileResourceStatus text_resize(XrCompileResources *r, void **out, size_t n) {
    if (fail_text && !injected) {
        injected = true;
        return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    }
    return xr_compile_resources_resize(r, out, n);
}
static XiCgenVerifyStatus publication_verify(XrCompileResources *r, const char *text, size_t length,
                                              const char *name) {
    if (clock_at_publication)
        clock_now = publication_clock;
    if (cancel_at_publication)
        *cancel_at_publication = true;
    if (corrupt_text) {
        static const char invalid[] = "int xray_lir_entry(void) {\n    return v9;\n}\n";
        return xr_compile_cgen_verify_output_or_ice(r, invalid, sizeof(invalid) - 1, name);
    }
    return xr_compile_cgen_verify_output_or_ice(r, text, length, name);
}
#define xr_compile_resources_calloc facts_allocate
#define xr_compile_resources_alloc workspace_allocate
#define xr_compile_resources_resize text_resize
#define xr_compile_cgen_verify_output_or_ice publication_verify
#include "lir/xlir_c.c"
#undef xr_compile_resources_calloc
#undef xr_compile_resources_alloc
#undef xr_compile_resources_resize
#undef xr_compile_cgen_verify_output_or_ice
#define OK(c) CHECK((c) == XR_LIR_OK)
#include "lir_array_region_loop_fixture.h"

static bool cancel_now(const void *context) { return *(const bool *) context; }

static void refusal(XrLirCRequest *request, XrLirCStatus expected) {
    XrLirCOutput output = {0};
    XrLirCDiagnostic diagnostic;
    uint64_t held = sdk_stats(request->resources).held_bytes;
    XrLirCStatus actual = xr_lir_emit_c(request, &output, &diagnostic);
    if (actual != expected)
        fprintf(stderr,"C refusal expected=%u actual=%u: %s\n",(unsigned)expected,(unsigned)actual,diagnostic.message);
    CHECK(actual == expected);
    CHECK(!output.bytes && !output.length && !output.entry_result);
    if (diagnostic.status != expected || sdk_stats(request->resources).held_bytes != held)
        fprintf(stderr, "C refusal expected=%u actual=%u held=%llu prior=%llu allocation=%zu: %s\n",
            (unsigned)expected, (unsigned)diagnostic.status, (unsigned long long)sdk_stats(request->resources).held_bytes,
            (unsigned long long)held, runtime_attempts, diagnostic.message);
    CHECK(diagnostic.status == expected && sdk_stats(request->resources).held_bytes == held);
}
static void control_boundaries(XrLirCRequest *request) {
    bool cancelled = true;
    XrCompileRequestOptions options = {cancel_now, &cancelled, XR_COMPILE_DEADLINE_NONE, 0};
    {
        XrCompileRequestControl control = {0};
        CHECK(xr_compile_request_control_init(&options, &control) == XR_COMPILE_RESOURCE_OK);
        request->control = &control;
        refusal(request, XR_LIR_C_CANCELLED);
        CHECK(control.status == XR_COMPILE_RESOURCE_CANCELLED);
        request->control = NULL;
    }
    {
        XrCompileRequestControl control = {0};
        cancelled = false; cancel_at_publication = &cancelled;
        CHECK(xr_compile_request_control_init(&options, &control) == XR_COMPILE_RESOURCE_OK);
        request->control = &control;
        refusal(request, XR_LIR_C_CANCELLED);
        CHECK(cancelled && control.status == XR_COMPILE_RESOURCE_CANCELLED);
        cancel_at_publication = NULL; request->control = NULL;
    }
    options = (XrCompileRequestOptions){NULL, NULL, XR_COMPILE_DEADLINE_AFTER, 0};
    {
        XrCompileRequestControl control = {0};
        CHECK(xr_compile_request_control_init(&options, &control) == XR_COMPILE_RESOURCE_OK);
        request->control = &control;
        size_t attempts = runtime_attempts;
        refusal(request, XR_LIR_C_DEADLINE);
        CHECK(runtime_attempts == attempts && control.status == XR_COMPILE_RESOURCE_DEADLINE);
        request->control = NULL;
    }
    clock_override = true;
    options.timeout_ns = 5;
    {
        XrCompileRequestControl control = {0};
        clock_now = 1000;
        CHECK(xr_compile_request_control_init(&options, &control) == XR_COMPILE_RESOURCE_OK);
        CHECK(control.deadline_ns == 1005);
        request->control = &control;
        clock_at_publication = true; publication_clock = control.deadline_ns;
        refusal(request, XR_LIR_C_DEADLINE);
        CHECK(control.deadline_ns == 1005 && control.status == XR_COMPILE_RESOURCE_DEADLINE);
        clock_at_publication = false; request->control = NULL;
    }
    {
        XrCompileRequestControl control = {0};
        clock_now = 0;
        CHECK(xr_compile_request_control_init(&options, &control) == XR_COMPILE_RESOURCE_CLOCK_FAILURE);
        CHECK(!control.initialized && !control.has_deadline && !control.deadline_ns);
        clock_now = 1000;
        CHECK(xr_compile_request_control_init(&options, &control) == XR_COMPILE_RESOURCE_OK);
        request->control = &control;
        clock_at_publication = true; publication_clock = 0;
        refusal(request, XR_LIR_C_CLOCK_FAILURE);
        CHECK(control.status == XR_COMPILE_RESOURCE_CLOCK_FAILURE);
        clock_at_publication = false; request->control = NULL;
    }
    {
        XrCompileRequestControl control = {0};
        clock_now = UINT64_MAX - 2;
        CHECK(xr_compile_request_control_init(&options, &control) == XR_COMPILE_RESOURCE_OVERFLOW);
        CHECK(!control.initialized && !control.deadline_ns);
    }
    clock_override = false;
    void *overflow = NULL;
    LirC c = {.request = request};
    CHECK(!c_resource(&c, xr_compile_resources_calloc(request->resources, SIZE_MAX, 2, &overflow)));
    CHECK(c.status == XR_LIR_C_OVERFLOW && !overflow);
    puts("real request cancellation, final publication deadline/clock failure, absolute deadline and overflow PASS");
}
static XrLirCOutput memory_boundaries(XrLirCRequest *request) {
    XrLirCOutput output = {0};
    XrCompileResources *original = request->resources;
    uint64_t baseline = sdk_stats(original).held_bytes;
    runtime_attempts = 0;
    CHECK(xr_lir_emit_c(request, &output, NULL) == XR_LIR_C_OK);
    size_t sites = runtime_attempts;
    XrCompileResourceStats measured = sdk_stats(original);
    CHECK(sites && measured.peak_bytes > baseline);
    xr_lir_c_output_free(&output);
    CHECK(sdk_stats(original).held_bytes == baseline);
    for (size_t i = 0; i < sites; ++i) {
        runtime_attempts = 0; runtime_fail_at = i;
        refusal(request, XR_LIR_C_OUT_OF_MEMORY);
        CHECK(runtime_attempts > i);
        runtime_fail_at = SIZE_MAX;
    }
    CHECK(measured.peak_bytes > 0 && measured.peak_bytes < UINT64_MAX);
    for (int adjustment = -1; adjustment <= 1; ++adjustment) {
        uint64_t limit = measured.peak_bytes;
        if (adjustment < 0) --limit;
        if (adjustment > 0) ++limit;
        XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, limit, true};
        XrCompileResources *finite = sdk_ledger(&limits);
        request->resources = finite;
        if (adjustment < 0) refusal(request, XR_LIR_C_MEMORY_LIMIT);
        else {
            CHECK(xr_lir_emit_c(request, &output, NULL) == XR_LIR_C_OK);
            xr_lir_c_output_free(&output);
        }
        xr_compile_resources_release(finite);
    }
    request->resources = original;
    bool *context = xr_malloc(sizeof(*context)); CHECK(context); *context = false;
    XrCompileRequestControl *control = xr_calloc(1, sizeof(*control)); CHECK(control);
    XrCompileRequestOptions options = {cancel_now, context, XR_COMPILE_DEADLINE_NONE, 0};
    CHECK(xr_compile_request_control_init(&options, control) == XR_COMPILE_RESOURCE_OK);
    request->control = control;
    CHECK(xr_lir_emit_c(request, &output, NULL) == XR_LIR_C_OK);
    memset(control, 0xa5, sizeof(*control)); memset(context, 0xa5, sizeof(*context));
    xr_free(control); xr_free(context); request->control = NULL;
    printf("actual C controlled allocator OOM sites=%zu; held peak=%llu minus1/exact/plus1 PASS\n",
        sites, (unsigned long long)measured.peak_bytes);
    return output;
}
#include "lir_record_fixture.h"
static void record_memory_boundaries(void) {
    XrLirFunction root = {0};
    XrLirModule *module = record_fixture(&root);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrCompileResources *resources = sdk_ledger(&limits);
    XrLirCRequest request = {module, root, resources, "record-publication-fault", NULL};
    uint64_t held = sdk_stats(resources).held_bytes;
    XrLirCOutput output = memory_boundaries(&request);
    xr_lir_module_destroy(module);
    CHECK(!strstr(output.bytes, "xrt_boxed_new") && !strstr(output.bytes, "xrt_boxed_field"));
    xr_lir_c_output_free(&output);
    CHECK(sdk_stats(resources).held_bytes == held);
    xr_compile_resources_release(resources);
}
static void literal_memory_boundaries(void) {
    XrLirModule *module = NULL;
    CHECK(xr_lir_module_create(NULL, &module) == XR_LIR_OK);
    const unsigned char bytes[] = {'A', 0, 0xc3, 0xa9};
    XrLirLiteral literal = {0};
    CHECK(xr_lir_add_literal(module, bytes, sizeof(bytes), &literal) == XR_LIR_OK);
    const XrLirType parameters[] = {XR_LIR_PTR, XR_LIR_U64};
    XrLirFunction print = {0}, entry = {0};
    XrLirFunctionSpec spec = {.name = "xrt_print_string", .name_length = 16,
        .parameters = parameters, .parameter_count = 2, .result = XR_LIR_UNIT,
        .kind = XR_LIR_FUNCTION_EXTERNAL, .extern_kind = XR_LIR_EXTERN_PRINT_STRING};
    CHECK(xr_lir_declare_function(module, &spec, &print) == XR_LIR_OK);
    spec = (XrLirFunctionSpec){.name = "literal", .name_length = 7, .result = XR_LIR_I64};
    CHECK(xr_lir_declare_function(module, &spec, &entry) == XR_LIR_OK);
    CHECK(xr_lir_define_function(module, entry) == XR_LIR_OK);
    XrLirBlock block = {0};
    CHECK(xr_lir_add_block(module, entry, NULL, 0, &block) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(module, entry, block) == XR_LIR_OK);
    XrLirValue arguments[2] = {{0}}, answer = {0};
    CHECK(xr_lir_literal_address(module, block, literal, &arguments[0]) == XR_LIR_OK);
    XrLirInstruction op = {.op = XR_LIR_CONST, .type = XR_LIR_U64, .bits = sizeof(bytes)};
    CHECK(xr_lir_emit_value(module, block, &op, &arguments[1]) == XR_LIR_OK);
    op = (XrLirInstruction){.op = XR_LIR_CALL, .type = XR_LIR_UNIT,
        .callee = print, .operands = arguments, .operand_count = 2};
    CHECK(xr_lir_emit_effect(module, block, &op) == XR_LIR_OK);
    op = (XrLirInstruction){.op = XR_LIR_CONST, .type = XR_LIR_I64, .bits = 42};
    CHECK(xr_lir_emit_value(module, block, &op, &answer) == XR_LIR_OK);
    op = (XrLirInstruction){.op = XR_LIR_RET, .type = XR_LIR_UNIT,
        .operands = &answer, .operand_count = 1};
    CHECK(xr_lir_set_terminator(module, block, &op) == XR_LIR_OK);
    CHECK(xr_lir_module_finish(module, NULL) == XR_LIR_OK);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrCompileResources *resources = sdk_ledger(&limits);
    XrLirCRequest request = {module, entry, resources, "literal-publication-fault", NULL};
    control_boundaries(&request);
    XrLirCOutput output = memory_boundaries(&request);
    xr_lir_module_destroy(module);
    xr_compile_resources_release(resources);
    memset(&request, 0xa5, sizeof(request));
    CHECK(strstr(output.bytes, "0x41,0x00,0xc3,0xa9,0") &&
          strstr(output.bytes, "xrt_print_string(") && output.entry_result == XR_LIR_I64);
    xr_lir_c_output_free(&output);
    CHECK(!runtime_live && !runtime_bytes);
    puts("literal used-set OOM/held/control and retired-producer output ownership PASS");
}
static void fixed_memory_boundaries(void) {
    XrLirModule *module = NULL;
    CHECK(xr_lir_module_create(NULL, &module) == XR_LIR_OK);
    XrLirType fixed = 0;
    CHECK(xr_lir_fixed_array_type(module, XR_LIR_U8, 17, &fixed) == XR_LIR_OK);
    XrLirFunction entry = {0};
    XrLirFunctionSpec spec = {.name = "fixed", .name_length = 5, .result = fixed};
    CHECK(xr_lir_declare_function(module, &spec, &entry) == XR_LIR_OK);
    CHECK(xr_lir_define_function(module, entry) == XR_LIR_OK);
    XrLirBlock block = {0};
    CHECK(xr_lir_add_block(module, entry, NULL, 0, &block) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(module, entry, block) == XR_LIR_OK);
    XrLirValue values[17] = {{0}}, aggregate = {0};
    for (uint32_t n = 0; n < 17; ++n) {
        XrLirInstruction byte = {.op = XR_LIR_CONST, .type = XR_LIR_U8, .bits = n};
        CHECK(xr_lir_emit_value(module, block, &byte, &values[n]) == XR_LIR_OK);
    }
    XrLirInstruction instruction = {.op = XR_LIR_FIXED_ARRAY_NEW, .type = fixed,
        .operands = values, .operand_count = 17};
    CHECK(xr_lir_emit_value(module, block, &instruction, &aggregate) == XR_LIR_OK);
    instruction = (XrLirInstruction){.op = XR_LIR_RET, .type = XR_LIR_UNIT,
        .operands = &aggregate, .operand_count = 1};
    CHECK(xr_lir_set_terminator(module, block, &instruction) == XR_LIR_OK);
    CHECK(xr_lir_module_finish(module, NULL) == XR_LIR_OK);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrCompileResources *resources = sdk_ledger(&limits);
    XrLirCRequest request = {module, entry, resources, "fixed-publication-fault", NULL};
    control_boundaries(&request);
    XrLirCOutput output = memory_boundaries(&request);
    xr_lir_module_destroy(module);
    xr_compile_resources_release(resources);
    memset(&request, 0xa5, sizeof(request));
    CHECK(output.entry_shape.kind == XR_LIR_TYPE_FIXED_ARRAY && output.entry_shape.element == XR_LIR_U8 &&
        output.entry_shape.count == 17 && output.entry_layout.size == 17 &&
        output.entry_layout.alignment == 1 && output.entry_layout.scalar_count == 17 &&
        !output.entry_layout.managed && strstr(output.bytes, "uint8_t bytes[17]"));
    xr_lir_c_output_free(&output);
    CHECK(!output.entry_shape.count && !output.entry_layout.size && !runtime_live && !runtime_bytes);
    puts("fixed type workspace FI and retired-producer result shape PASS");
}

static void managed_string_boundaries(const char *path) {
    XrLirModule *module = NULL;
    CHECK(xr_lir_module_create(NULL, &module) == XR_LIR_OK);
    XrLirLiteral literal = {0};
    const unsigned char bytes[] = {'A', 0, 0xc3, 0xa9};
    CHECK(xr_lir_add_literal(module, bytes, sizeof(bytes), &literal) == XR_LIR_OK);
    XrLirFunction entry = {0}; XrLirBlock block = {0};
    XrLirFunctionSpec spec = {.name="owned", .name_length=5, .result=XR_LIR_STRING};
    CHECK(xr_lir_declare_function(module, &spec, &entry) == XR_LIR_OK);
    CHECK(xr_lir_define_function(module, entry) == XR_LIR_OK);
    CHECK(xr_lir_add_block(module, entry, NULL, 0, &block) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(module, entry, block) == XR_LIR_OK);
    XrLirValue input = {0}, copy = {0}, joined = {0};
    XrLirInstruction op = {.op=XR_LIR_STRING_LITERAL, .type=XR_LIR_STRING, .bits=literal.id};
    CHECK(xr_lir_emit_value(module, block, &op, &input) == XR_LIR_OK);
    op = (XrLirInstruction){.op=XR_LIR_STRING_RETAIN, .type=XR_LIR_STRING, .operands=&input, .operand_count=1};
    CHECK(xr_lir_emit_value(module, block, &op, &copy) == XR_LIR_OK);
    XrLirValue inputs[] = {input, copy};
    op = (XrLirInstruction){.op=XR_LIR_STRING_CONCAT, .type=XR_LIR_STRING, .operands=inputs, .operand_count=2};
    CHECK(xr_lir_emit_value(module, block, &op, &joined) == XR_LIR_OK);
    for (unsigned i = 0; i < 2; ++i) {
        op = (XrLirInstruction){.op=XR_LIR_STRING_DROP, .type=XR_LIR_UNIT, .operands=&inputs[i], .operand_count=1};
        CHECK(xr_lir_emit_effect(module, block, &op) == XR_LIR_OK);
    }
    op = (XrLirInstruction){.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&joined, .operand_count=1};
    CHECK(xr_lir_set_terminator(module, block, &op) == XR_LIR_OK);
    CHECK(xr_lir_module_finish(module, NULL) == XR_LIR_OK);
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XrLirCRequest request = {module, entry, resources, "managed-string-publication", NULL};
    control_boundaries(&request);
    XrLirCOutput output = memory_boundaries(&request);
    CHECK(strstr(output.bytes, "xrt_native_string_create(") && strstr(output.bytes, "xrt_managed_slot_replace(") &&
        strstr(output.bytes, "xrt_native_string_concat_create(") && !strstr(output.bytes, "xrt_string_retain("));
    xr_lir_module_destroy(module); xr_compile_resources_release(resources);
    memset(&request, 0xa5, sizeof(request));
    FILE *file = fopen(path, "wb");
    CHECK(file && fwrite(output.bytes, 1, output.length, file) == output.length && fclose(file) == 0);
    xr_lir_c_output_free(&output);
    CHECK(!runtime_live && !runtime_bytes);
    puts("managed String receiving metadata, actual allocation faults and producer retirement PASS");
}
typedef struct EdgeControl {
    unsigned polls, trigger, mode;
} EdgeControl;
static bool edge_interrupt(const void *context) {
    EdgeControl *control = (EdgeControl *)context;
    if (++control->polls != control->trigger) return false;
    if (control->mode) clock_now = control->mode == 1 ? 1005 : 0;
    return !control->mode;
}
static void edge_control_boundaries(void) {
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XrLirHomeEvent events[6] = {{0}};
    for (size_t i = 0; i < 6; ++i) events[i].kind = XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER;
    XrLirValueHomes homes = {.events=events, .event_count=6};
    const XrLirCStatus failures[] = {XR_LIR_C_CANCELLED, XR_LIR_C_DEADLINE, XR_LIR_C_CLOCK_FAILURE};
    for (unsigned mode = 0; mode < 3; ++mode) for (unsigned statistic = 2; statistic <= 3; ++statistic) {
        EdgeControl interrupted = {.trigger=statistic + (mode == 0), .mode=mode};
        clock_override = true; clock_now = 1000;
        XrCompileRequestOptions options = {edge_interrupt, &interrupted,
            mode ? XR_COMPILE_DEADLINE_AFTER : XR_COMPILE_DEADLINE_NONE, mode ? 5 : 0};
        XrCompileRequestControl control = {0};
        CHECK(xr_compile_request_control_init(&options, &control) == XR_COMPILE_RESOURCE_OK);
        XrLirCRequest request = {.resources=resources, .translation_unit="edge-control", .control=&control};
        LirC c = {.request=&request}; LirCEdgeEvents index = {0};
        size_t attempts = runtime_attempts; uint64_t held = sdk_stats(resources).held_bytes;
        c_edge_events_prepare(&c, &homes, &index);
        CHECK(c.status == failures[mode] && !index.index && !index.capacity);
        CHECK(runtime_attempts == attempts && sdk_stats(resources).held_bytes == held);
        clock_override = false;
    }
    xr_compile_resources_release(resources); CHECK(!runtime_live && !runtime_bytes);
    puts("edge statistics mid-loop cancellation, deadline and clock failure allocate nothing and refund exactly PASS");
}
#include "lir_c_managed_state_fixture.h"
static void managed_state_boundaries(const char *path) {
    XrLirFunction root = {0}; XrLirModule *module = managed_state_fixture(&root);
    XrCompileResources *resources = sdk_ledger(&sdk_unlimited);
    XrLirCRequest request = {module, root, resources, "managed-State-publication", NULL};
    control_boundaries(&request);
    XrLirCOutput output = memory_boundaries(&request);
    CHECK(strstr(output.bytes, "XrtValueImage xrl_value_image") && strstr(output.bytes, "XrtModuleLayoutSpec xrl_state_spec") &&
        strstr(output.bytes, "source_access_") && !strstr(output.bytes, "XrtModuleTypeSpec"));
    xr_lir_module_destroy(module); xr_compile_resources_release(resources);
    memset(&request, 0xa5, sizeof(request));
    FILE *file = fopen(path, "wb");
    CHECK(file && fwrite(output.bytes, 1, output.length, file) == output.length && fclose(file) == 0);
    xr_lir_c_output_free(&output);
    CHECK(!runtime_live && !runtime_bytes);
    puts("State images, mixed String/reference cache edges, allocator faults and producer retirement PASS");
}
static void cell_memory_boundaries(void) {
    XrLirModule *module = NULL; CHECK(xr_lir_module_create(NULL,&module) == XR_LIR_OK);
    XrLirType cell = 0; CHECK(xr_lir_cell_type(module,(XrLirCellType){XR_LIR_I64,0},&cell) == XR_LIR_OK);
    XrLirFunction entry = {0}; XrLirBlock block = {0};
    CHECK(xr_lir_declare_function(module,&(XrLirFunctionSpec){.name="cell",.name_length=4,.result=XR_LIR_I64},&entry) == XR_LIR_OK);
    CHECK(xr_lir_define_function(module,entry) == XR_LIR_OK);
    CHECK(xr_lir_add_block(module,entry,NULL,0,&block) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(module,entry,block) == XR_LIR_OK);
    XrLirValue seed = {0}, identity = {0}, copied = {0}, result = {0};
    CHECK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=42},&seed) == XR_LIR_OK);
    CHECK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=cell,.operands=&seed,.operand_count=1},&identity) == XR_LIR_OK);
    CHECK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_VALUE_RETAIN,.type=cell,.operands=&identity,.operand_count=1},&copied) == XR_LIR_OK);
    CHECK(xr_lir_emit_value(module,block,&(XrLirInstruction){.op=XR_LIR_CELL_READ,.type=XR_LIR_I64,.operands=&copied,.operand_count=1},&result) == XR_LIR_OK);
    CHECK(xr_lir_emit_effect(module,block,&(XrLirInstruction){.op=XR_LIR_VALUE_DROP,.type=XR_LIR_UNIT,.operands=&copied,.operand_count=1}) == XR_LIR_OK);
    CHECK(xr_lir_set_terminator(module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,.operands=&result,.operand_count=1}) == XR_LIR_OK);
    CHECK(xr_lir_module_finish(module,NULL) == XR_LIR_OK);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrCompileResources *resources = sdk_ledger(&limits);
    XrLirCRequest request = {module,entry,resources,"cell-publication-fault",NULL};
    control_boundaries(&request); XrLirCOutput output = memory_boundaries(&request);
    xr_lir_module_destroy(module); xr_compile_resources_release(resources);
    CHECK(strstr(output.bytes,"xrt_native_cell_create") && strstr(output.bytes,"xrt_native_cell_i64_read") &&
        strstr(output.bytes,"xrt_inline_value_replace") && output.entry_result == XR_LIR_I64);
    xr_lir_c_output_free(&output); CHECK(!runtime_live && !runtime_bytes);
}
static void map_literal_memory_boundaries(void) {
    XrLirModule *module = NULL; CHECK(xr_lir_module_create(NULL, &module) == XR_LIR_OK);
    char path[] = "map-fault.xr";
    XrLirMapLiteral literal = {.path=path, .path_length=(uint32_t)sizeof(path) - 1};
    uint32_t ordinal = 0; CHECK(xr_lir_map_literal_add(module, &literal, &ordinal) == XR_LIR_OK);
    path[0] = 'z';
    XrLirFunction entry = {0}; XrLirBlock block = {0};
    CHECK(xr_lir_declare_function(module, &(XrLirFunctionSpec){.name="map", .name_length=3,
        .result=XR_LIR_I64}, &entry) == XR_LIR_OK);
    CHECK(xr_lir_define_function(module, entry) == XR_LIR_OK);
    CHECK(xr_lir_add_block(module, entry, NULL, 0, &block) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(module, entry, block) == XR_LIR_OK);
    XrLirValue map = {0}, answer = {0};
    CHECK(xr_lir_emit_value(module, block, &(XrLirInstruction){.op=XR_LIR_MAP_LITERAL_NEW,
        .type=XR_LIR_MAP_STRING_I64, .bits=ordinal}, &map) == XR_LIR_OK);
    CHECK(xr_lir_emit_effect(module, block, &(XrLirInstruction){.op=XR_LIR_MAP_DROP,
        .type=XR_LIR_UNIT, .operands=&map, .operand_count=1}) == XR_LIR_OK);
    CHECK(xr_lir_emit_value(module, block, &(XrLirInstruction){.op=XR_LIR_CONST,
        .type=XR_LIR_I64, .bits=42}, &answer) == XR_LIR_OK);
    CHECK(xr_lir_set_terminator(module, block, &(XrLirInstruction){.op=XR_LIR_RET,
        .type=XR_LIR_UNIT, .operands=&answer, .operand_count=1}) == XR_LIR_OK);
    CHECK(xr_lir_module_finish(module, NULL) == XR_LIR_OK);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrCompileResources *resources = sdk_ledger(&limits);
    XrLirCRequest request = {module, entry, resources, "map-literal-publication-fault", NULL};
    control_boundaries(&request); XrLirCOutput output = memory_boundaries(&request);
    xr_lir_module_destroy(module); xr_compile_resources_release(resources);
    CHECK(strstr(output.bytes, "xrt_native_map_literal_create") && strstr(output.bytes, "map-fault.xr") &&
        !strstr(output.bytes, "zap-fault.xr") && output.entry_result == XR_LIR_I64);
    xr_lir_c_output_free(&output); CHECK(!runtime_live && !runtime_bytes);
}
static XrLirFunction array_loop_test_root(ArraySetRegionFixture *input) {
    static const char name[] = "array_loop_test_root";
    XrLirFunction root = {0}; XrLirBlock block = {0}; XrLirValue start = {0}, result = {0};
    OK(xr_lir_declare_function(input->module,&(XrLirFunctionSpec){.name=name,.name_length=sizeof(name) - 1,
        .result=XR_LIR_I64},&root));
    OK(xr_lir_define_function(input->module,root));
    OK(xr_lir_add_block(input->module,root,NULL,0,&block)); OK(xr_lir_set_entry(input->module,root,block));
    OK(xr_lir_emit_value(input->module,block,&(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=0},&start));
    OK(xr_lir_emit_value(input->module,block,&(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_I64,
        .callee=input->function,.operands=&start,.operand_count=1},&result));
    OK(xr_lir_set_terminator(input->module,block,&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
        .operands=&result,.operand_count=1}));
    return root;
}
typedef struct ArrayCloneCancelPoint {
    const LirC *compiler;
    bool after_read, reached;
    size_t attempts, emitted;
} ArrayCloneCancelPoint;
static bool array_clone_cancel(const void *context) {
    ArrayCloneCancelPoint *point = (ArrayCloneCancelPoint *)context;
    if (point->after_read && (!point->compiler->text ||
            !strstr(point->compiler->text, "XRT_ARRAY_I64_REGION_COPY(&v"))) return false;
    point->reached = true;
    point->emitted = point->compiler->length;
    point->attempts = runtime_attempts;
    return true;
}
static bool array_local_index_cancel(const void *context) {
    ArrayLocalIndexCancelPoint *point = (ArrayLocalIndexCancelPoint *)context;
    if (!point->index) return false;
    point->populated = 0;
    for (size_t n = 0; n < point->count; ++n) point->populated += point->index[n] != 0;
    if (!point->populated) return false;
    CHECK(sdk_stats(point->resources).held_bytes > point->held && runtime_live == point->live + 1 &&
        runtime_bytes > point->bytes);
    point->reached = true; point->attempts = runtime_attempts; return true;
}
static void array_local_index_cancellation(XrLirCRequest *request, const XrLirValueHomes *homes,
    const XrLirValueLayouts *layouts) {
    uint64_t held = sdk_stats(request->resources).held_bytes; size_t live = runtime_live, bytes = runtime_bytes;
    LirCValueTypes types = {0};
    LirC compiler = {.request=request,.function={request->module,homes->source_function},.homes=homes,.value_types=&types};
    c_value_types_prepare(&compiler,layouts,&types);
    CHECK(compiler.status == XR_LIR_C_OK && homes->workspace_count > 1 && c_array_local_function(&compiler,homes));
    compiler.array_local = true;
    ArrayLocalIndexCancelPoint point = {.resources=request->resources};
    XrCompileRequestControl control = {0};
    XrCompileRequestOptions options = {array_local_index_cancel,&point,XR_COMPILE_DEADLINE_NONE,0};
    CHECK(xr_compile_request_control_init(&options,&control) == XR_COMPILE_RESOURCE_OK);
    request->control = &control; CHECK(!array_local_index_watch); array_local_index_watch = &point;
    c_array_local_specs(&compiler);
    array_local_index_watch = NULL; request->control = NULL;
    CHECK(point.reached && point.count / 2 >= homes->workspace_count && point.populated &&
        point.populated < homes->workspace_count && compiler.status == XR_LIR_C_CANCELLED &&
        control.status == XR_COMPILE_RESOURCE_CANCELLED && runtime_attempts == point.attempts);
    /* The caller returned the held index even though population stopped.
     * Do not dereference its expired address; only the original text remains. */
    CHECK(sdk_stats(request->resources).held_bytes == point.held && runtime_live == point.live && runtime_bytes == point.bytes);
    CHECK(compiler.text && compiler.length);
    xr_compile_resources_free(compiler.text);
    xr_compile_resources_free(types.index); xr_compile_resources_free(types.rows);
    CHECK(sdk_stats(request->resources).held_bytes == held && runtime_live == live && runtime_bytes == bytes);
}
static void array_local_index_fixture(XrCompileResources *resources) {
    /* The scalar projection fixture has eight blocks but only one SET. Two
     * existing loops supply two actual workspaces, so cancellation can stop
     * index population after a real insertion and before it is complete. */
    ArraySetRegionFixture input = array_region_binding_two_loops(false,false,true);
    XrtBillingOwner *billing = NULL; CHECK(xrt_billing_owner_new(NULL,SIZE_MAX,&billing) == XRT_MANAGED_OK);
    XrLirValueLayoutKey keys[] = {{XR_LIR_UNIT,0,0},{XR_LIR_I1,0,0},{XR_LIR_I64,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0}};
    XrLirValueLayouts *layouts = NULL;
    CHECK(xr_lir_value_layouts_create(input.module,keys,4,billing,&layouts) == XR_LIR_VALUE_LAYOUT_OK);
    XrLirOwnershipPlan *plan = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=input.module},&plan,NULL));
    XrLirValueHomes *homes = NULL;
    CHECK(xr_lir_value_homes_create(&(XrLirValueHomesRequest){input.module,input.function,layouts,plan},&homes) == XR_LIR_VALUE_LAYOUT_OK);
    XrLirFunctionView root = {0}; OK(xr_lir_function_view(input.module,input.function,&root));
    CHECK(!root.parameter_count && homes->source_function == input.function.id);
    XrLirCRequest request = {input.module,input.function,resources,"array-local-index-cancellation",NULL};
    array_local_index_cancellation(&request,homes,layouts);
    xr_lir_value_homes_destroy(homes); xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(layouts);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
    xr_lir_module_destroy(input.module);
}
static void array_clone_cancellation(void) {
    ArraySetRegionFixture input = ars_range_build(NULL,12,ARRAY_REGION_RANGE_READ_ONLY);
    XrLirFunction root = array_loop_test_root(&input); OK(xr_lir_module_finish(input.module,NULL));
    XrtBillingOwner *billing = NULL; CHECK(xrt_billing_owner_new(NULL,SIZE_MAX,&billing) == XRT_MANAGED_OK);
    XrLirValueLayoutKey keys[] = {{XR_LIR_UNIT,0,0},{XR_LIR_I1,0,0},{XR_LIR_I64,0,0},{XR_LIR_ARRAY,XR_LIR_I64,0}};
    XrLirValueLayouts *layouts = NULL;
    CHECK(xr_lir_value_layouts_create(input.module,keys,4,billing,&layouts) == XR_LIR_VALUE_LAYOUT_OK);
    XrLirOwnershipPlan *plan = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=input.module},&plan,NULL));
    XrLirValueHomes *homes = NULL;
    CHECK(xr_lir_value_homes_create(&(XrLirValueHomesRequest){input.module,input.function,layouts,plan},
        &homes) == XR_LIR_VALUE_LAYOUT_OK);
    CHECK(homes->fixed_array_update_count == 1 &&
        homes->fixed_array_updates[0].region.primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_GET);
    XrLirValueHomes *function_homes[2] = {0};
    CHECK(xr_lir_function_count(input.module) == 2 && input.function.id && input.function.id <= 2);
    function_homes[input.function.id - 1] = homes;
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY,0,true};
    XrCompileResources *resources = sdk_ledger(&limits);
    uint64_t held = sdk_stats(resources).held_bytes; size_t live = runtime_live, bytes = runtime_bytes;
    for (unsigned n = 0; n < 2; ++n) {
        XrLirCRequest request = {input.module,root,resources,"array-clone-cancellation",NULL};
        LirC compiler = {.request=&request,.function=input.function,.value_homes=function_homes,.homes=homes};
        OK(xr_lir_function_view(input.module,input.function,&compiler.view));
        compiler.scratch = compiler.view.value_count;
        void *memory = NULL;
        CHECK(xr_compile_resources_calloc(resources,2,sizeof(*compiler.array_regions),&memory) ==
            XR_COMPILE_RESOURCE_OK);
        compiler.array_regions = memory;
        compiler.array_region = &compiler.array_regions[input.function.id - 1];
        c_fixed_array_prepare(&compiler,input.function.id,&compiler.array_regions[input.function.id - 1]);
        CHECK(compiler.status == XR_LIR_C_OK && compiler.array_region->fixed_edges);
        ArrayCloneCancelPoint point = {.compiler=&compiler,.after_read=n != 0};
        XrCompileRequestControl control = {0};
        XrCompileRequestOptions options = {array_clone_cancel,&point,XR_COMPILE_DEADLINE_NONE,0};
        CHECK(xr_compile_request_control_init(&options,&control) == XR_COMPILE_RESOURCE_OK);
        request.control = &control;
        c_fixed_array_clones(&compiler);
        CHECK(point.reached && compiler.status == XR_LIR_C_CANCELLED && control.status == XR_COMPILE_RESOURCE_CANCELLED);
        CHECK(compiler.length == point.emitted && runtime_attempts == point.attempts);
        CHECK((point.emitted != 0) == point.after_read && (compiler.text != NULL) == point.after_read);
        if (compiler.text) CHECK(!strstr(compiler.text,"goto xrl_fixed_exit_1;"));
        c_array_regions_destroy(&compiler);
        xr_compile_resources_free(compiler.text);
        CHECK(sdk_stats(resources).held_bytes == held && runtime_live == live && runtime_bytes == bytes);
    }
    array_local_index_fixture(resources);
    CHECK(sdk_stats(resources).held_bytes == held && runtime_live == live && runtime_bytes == bytes);
    xr_compile_resources_release(resources); xr_lir_value_homes_destroy(homes);
    xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(layouts);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
    xr_lir_module_destroy(input.module); CHECK(!runtime_live && !runtime_bytes);
}
static const char *array_fixed_c_marker(const char *text, const char *pattern, uint32_t number) {
    char marker[96];
    int length = snprintf(marker,sizeof(marker),pattern,number);
    CHECK(length > 0 && (size_t)length < sizeof(marker));
    const char *at = strstr(text,marker); CHECK(at);
    return at;
}

static const char *array_fixed_update(const char *text, uint32_t loop) {
    const char *reset = array_fixed_c_marker(text,"\n    xrl_fixed_tried_%" PRIu32 " = 0;",loop);
    const char *entry = strstr(reset,"xrt_array_i64_region_try_begin(&xrl_fixed_context, &xrl_fixed_spec, true,");
    const char *entry_target = array_fixed_c_marker(reset,"goto xrl_fixed_%" PRIu32 "_bb",loop);
    const char *guard = array_fixed_c_marker(text,"if (!xrl_fixed_tried_%" PRIu32 ") {",loop);
    const char *begin = strstr(guard,"xrt_array_i64_region_try_begin(&xrl_fixed_context, &xrl_fixed_spec, true,");
    const char *enter = array_fixed_c_marker(guard,"goto xrl_fixed_%" PRIu32 "_bb",loop);
    const char *clone = array_fixed_c_marker(enter,"\nxrl_fixed_%" PRIu32 "_bb",loop);
    const char *leave = array_fixed_c_marker(clone,"goto xrl_fixed_exit_%" PRIu32 ";",loop);
    CHECK(entry && entry < entry_target && entry_target < guard);
    CHECK(begin && begin < enter && enter < clone && clone < leave);
    CHECK(!strstr(guard + 1,"if (!xrl_fixed_tried_1) {") || loop != 1);
    const char *read = strstr(clone,"XRT_ARRAY_I64_REGION_COPY(&v");
    const char *write = array_fixed_c_marker(clone,"XRT_ARRAY_I64_REGION_COPY(xrl_fixed_cache_%" PRIu32 ".elements",loop);
    const char *release = array_fixed_c_marker(clone,"xrl_fixed_cache_%" PRIu32 " = (XrtArrayI64RegionCache){0};",loop);
    const char *ordinary_call = strstr(clone,"xrt_");
    CHECK(read && read < write && write < release && release < leave && (!ordinary_call || ordinary_call > leave));
    (void)array_fixed_c_marker(text,"\nxrl_fixed_exit_%" PRIu32 ": ;",loop);
    return clone;
}

static void array_fixed_first_write(const char *text, XrLirBlock body) {
    const char *block = array_fixed_c_marker(text,"\nbb%" PRIu32 ": ;",body.id);
    const char *next = strstr(block + 1,"\nbb");
    const char *ordinary = strstr(block,"xrt_native_array_update_place(");
    CHECK(next && ordinary && ordinary < next);
}

static void array_fixed_header_liveout(const char *text, const char *clone, const ArraySetRegionFixture *input) {
    XrLirInstructionView length_op = {0};
    OK(xr_lir_instruction_view(input->module,input->header,0,&length_op));
    char assignment[96];
    int length = snprintf(assignment,sizeof(assignment),"v%" PRIu32 " = (int64_t)xrl_fixed_cache_1.length;",
        length_op.result);
    CHECK(length > 0 && (size_t)length < sizeof(assignment));
    const char *inside = strstr(clone,assignment);
    const char *after = inside ? strstr(inside + 1,assignment) : NULL;
    const char *leave = strstr(clone,"goto xrl_fixed_exit_1;");
    const char *write = strstr(clone,"XRT_ARRAY_I64_REGION_COPY(xrl_fixed_cache_1.elements");
    CHECK(inside && write && after && leave && inside < write && write < after && after < leave);
    const char *formal = strstr(clone,"(uint64_t)p0");
    CHECK(formal && formal < write);

    XrLirBlockView exit_block = {0};
    OK(xr_lir_block_view(input->module,input->exit,&exit_block));
    CHECK(exit_block.parameter_count > 1);
    const char *edge = strstr(text,"\nxrl_fixed_exit_1: ;"); CHECK(edge);
    const char *target = array_fixed_c_marker(edge,"goto bb%" PRIu32 ";",input->exit.id);
    length = snprintf(assignment,sizeof(assignment),"v%" PRIu32 " = v",exit_block.parameters[1]);
    CHECK(length > 0 && (size_t)length < sizeof(assignment));
    const char *scalar = strstr(edge,assignment);
    CHECK(scalar && scalar < target);
}

static void array_checked_clone(const char *text, uint32_t loop, bool pointwise, bool readonly) {
    const char *guard = array_fixed_c_marker(text," &xrl_fixed_cache_%" PRIu32 ")",loop);
    const char *entry = array_fixed_c_marker(guard,"goto xrl_fixed_%" PRIu32 "_bb",loop);
    const char *clone = array_fixed_c_marker(entry,"\nxrl_fixed_%" PRIu32 "_bb",loop);
    const char *leave = array_fixed_c_marker(clone,"goto xrl_fixed_exit_%" PRIu32 ";",loop);
    const char *release = array_fixed_c_marker(clone,"xrl_fixed_cache_%" PRIu32 " = (XrtArrayI64RegionCache){0};",loop);
    const char *read = strstr(clone,"XRT_ARRAY_I64_REGION_COPY(&v");
    const char *write = strstr(clone,"XRT_ARRAY_I64_REGION_COPY(xrl_fixed_cache_");
    const char *runtime = strstr(clone,"xrt_");
    CHECK(entry < clone && read && read < leave && release < leave && (!runtime || runtime > leave));
    CHECK((write && write < leave) == !readonly);
    if (pointwise) {
        const char *check = array_fixed_c_marker(clone," >= xrl_fixed_cache_%" PRIu32 ".length) {",loop);
        const char *slow = strstr(check,"goto xrl_fixed_slow_");
        CHECK(check < release && slow && release < slow && slow < read);
    } else {
        CHECK(read < release);
    }
    (void)array_fixed_c_marker(text,"\nxrl_fixed_exit_%" PRIu32 ": ;",loop);
}
static void array_loop_boundaries(void) {
    const enum ArrayRegionRangeMode modes[] = {ARRAY_REGION_RANGE_UNIT,ARRAY_REGION_RANGE_C_HEADER_LIVEOUT,
        ARRAY_REGION_RANGE_NEXT_COPIES,ARRAY_REGION_RANGE_STEP_TWO,ARRAY_REGION_RANGE_PARALLEL_BAD,ARRAY_REGION_RANGE_READ_ONLY};
    (void)ars_range_same; (void)ars_range_digest;
    (void)array_set_region_fixture;
    size_t mode_count = sizeof(modes) / sizeof(*modes);
    for (size_t n = 0; n < mode_count + 3; ++n) {
        bool dual = n >= mode_count, mixed = n == mode_count + 1, shared = n == mode_count + 2;
        bool readonly = !dual && modes[n] == ARRAY_REGION_RANGE_READ_ONLY;
        bool pointwise = !dual && (modes[n] == ARRAY_REGION_RANGE_STEP_TWO ||
            modes[n] == ARRAY_REGION_RANGE_PARALLEL_BAD);
        ArraySetRegionFixture input = mixed ? array_region_binding_loops(false,false,true,2,false) :
            dual ? array_region_binding_two_loops(shared,false,true) :
            ars_range_build(NULL,n == 0 ? 8 : 3,modes[n]);
        XrLirFunction root = dual ? input.function : array_loop_test_root(&input);
        if (!dual) OK(xr_lir_module_finish(input.module,NULL));
        XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY,0,true};
        XrCompileResources *resources = sdk_ledger(&limits);
        XrLirCRequest request = {input.module,root,resources,"array-loop-publication",NULL};
        XrLirCOutput output = {0};
        if (!n || readonly) output = memory_boundaries(&request);
        else {
            XrLirCDiagnostic diagnostic = {0};
            XrLirCStatus status = xr_lir_emit_c(&request,&output,&diagnostic);
            if (status != XR_LIR_C_OK) fprintf(stderr,"array loop status%u: %s\n",(unsigned)status,diagnostic.message);
            CHECK(status == XR_LIR_C_OK);
        }

        CHECK((strstr(output.bytes,"\nxrl_fixed_exit_2: ;") != NULL) == dual);
        CHECK(!strstr(output.bytes,"\nxrl_fixed_exit_3: ;"));
        for (uint32_t loop = 1; loop <= (dual ? 2u : 1u); ++loop) {
            bool read_loop = readonly || (mixed && loop == 2);
            if (read_loop || pointwise) array_checked_clone(output.bytes,loop,pointwise,read_loop);
            else {
                const char *clone = array_fixed_update(output.bytes,loop);
                if (n == 1) array_fixed_header_liveout(output.bytes,clone,&input);
            }
        }
        CHECK(strstr(output.bytes,"xrt_native_array_get("));
        if (!readonly && !mixed) array_fixed_first_write(output.bytes,input.body[0]);
        xr_lir_module_destroy(input.module); xr_compile_resources_release(resources);
        CHECK(output.bytes && output.length); xr_lir_c_output_free(&output);
        CHECK(!runtime_live && !runtime_bytes);
    }
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--edge-control")) {
        edge_control_boundaries(); return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--managed-state")) {
        managed_state_boundaries(argv[2]); return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--managed-string")) {
        edge_control_boundaries();
        managed_string_boundaries(argv[2]); return 0;
    }
    if (argc == 1) edge_control_boundaries();
    if (argc == 1) record_memory_boundaries();
    if (argc == 1) array_loop_boundaries();
    if (argc == 1) array_clone_cancellation();
    (void)sdk_fixture_malloc; (void)sdk_fixture_free;
    CHECK(argc == 1 || (argc == 2 && !strcmp(argv[1], "--ice")));
    XrLirModule *module = NULL;
    XrLirFunction entry;
    XrLirBlock block;
    XrLirValue value;
    XrLirFunctionSpec function = {.name = "root", .name_length = 4, .result = XR_LIR_I64};
    CHECK(xr_lir_module_create(NULL, &module) == XR_LIR_OK);
    CHECK(xr_lir_declare_function(module, &function, &entry) == XR_LIR_OK);
    CHECK(xr_lir_define_function(module, entry) == XR_LIR_OK);
    CHECK(xr_lir_add_block(module, entry, NULL, 0, &block) == XR_LIR_OK);
    CHECK(xr_lir_set_entry(module, entry, block) == XR_LIR_OK);
    XrLirInstruction constant = {.op = XR_LIR_CONST, .type = XR_LIR_I64, .bits = 42};
    CHECK(xr_lir_emit_value(module, block, &constant, &value) == XR_LIR_OK);
    XrLirValue divisor;
    constant.bits = 0;
    CHECK(xr_lir_emit_value(module, block, &constant, &divisor) == XR_LIR_OK);
    XrLirValue operands[] = {value, divisor};
    XrLirInstruction division = {.op = XR_LIR_DIV, .type = XR_LIR_I64,
        .operands = operands, .operand_count = 2};
    CHECK(xr_lir_emit_value(module, block, &division, &value) == XR_LIR_OK);
    static const char last_point[] = "last-instruction.xr";
    XrLirInstruction ret = {.op = XR_LIR_RET, .type = XR_LIR_UNIT, .operands = &value, .operand_count = 1,
        .source = {last_point, sizeof(last_point) - 1, 9, 2, 9, 2}};
    CHECK(xr_lir_set_terminator(module, block, &ret) == XR_LIR_OK);
    CHECK(xr_lir_module_finish(module, NULL) == XR_LIR_OK);
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, true};
    XrCompileResources *resources = sdk_ledger(&limits);
    XrLirCRequest request = {module, entry, resources, "publication-fault", NULL};
    XrLirCOutput output = {0};
    XrLirCDiagnostic diagnostic;
    if (argc == 2) {
        corrupt_text = true;
        (void) xr_lir_emit_c(&request, &output, &diagnostic);
        puts("UNEXPECTED_PUBLICATION");
        return 1;
    }
    for (unsigned point = 0; point < 5; ++point) {
        fail_workspace = point == 0;
        fail_text = point == 1;
        fail_divisors = point >= 2;
        divisor_attempt = 0;
        failed_divisor_attempt = point >= 2 ? point - 1 : 0;
        injected = false;
        CHECK(xr_lir_emit_c(&request, &output, &diagnostic) == XR_LIR_C_OUT_OF_MEMORY);
        CHECK(injected && !output.bytes && !output.length && !output.entry_result);
        CHECK(diagnostic.status == XR_LIR_C_OUT_OF_MEMORY);
        if (fail_divisors)
            CHECK(diagnostic.function == entry.id && !diagnostic.instruction && !diagnostic.source.path);
        fail_workspace = fail_text = fail_divisors = false;
        CHECK(xr_lir_emit_c(&request, &output, &diagnostic) == XR_LIR_C_OK);
        xr_lir_c_output_free(&output);
    }
    control_boundaries(&request);
    output = memory_boundaries(&request);
    xr_lir_module_destroy(module);
    xr_compile_resources_release(resources);
    memset(&request, 0xa5, sizeof(request));
    CHECK(strstr(output.bytes, "xray_lir_entry(void)") && output.entry_result == XR_LIR_I64);
    xr_lir_c_output_free(&output);
    CHECK(!output.bytes && !output.length && !output.entry_result && !runtime_live && !runtime_bytes);
    literal_memory_boundaries();
    fixed_memory_boundaries();
    cell_memory_boundaries();
    map_literal_memory_boundaries();
    puts("representative C workspace/text failures and cancellation passed");
    puts("output survives module, caller memory reference, control/context destruction; physical zero PASS");
    return 0;
}
