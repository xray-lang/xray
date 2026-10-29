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
#include "aot/xi_cgen_verify_output.h"
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
    return xr_compile_resources_calloc(r, n, size, out);
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

static bool cancel_now(const void *context) { return *(const bool *) context; }

static void refusal(XrLirCRequest *request, XrLirCStatus expected) {
    XrLirCOutput output = {0};
    XrLirCDiagnostic diagnostic;
    uint64_t held = sdk_stats(request->resources).held_bytes;
    CHECK(xr_lir_emit_c(request, &output, &diagnostic) == expected);
    CHECK(!output.bytes && !output.length && !output.entry_result);
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
int main(int argc, char **argv) {
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
    puts("representative C workspace/text failures and cancellation passed");
    puts("output survives module, caller memory reference, control/context destruction; physical zero PASS");
    return 0;
}
