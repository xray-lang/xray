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
static bool fail_workspace, fail_text, fail_divisors, corrupt_text, injected;
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
    if (fail_divisors && size == 1 && n == 4 && !injected) {
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

int main(int argc, char **argv) {
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
    constant.bits = 3;
    CHECK(xr_lir_emit_value(module, block, &constant, &divisor) == XR_LIR_OK);
    XrLirValue operands[] = {value, divisor};
    XrLirInstruction division = {.op = XR_LIR_DIV, .type = XR_LIR_I64,
        .operands = operands, .operand_count = 2};
    CHECK(xr_lir_emit_value(module, block, &division, &value) == XR_LIR_OK);
    XrLirInstruction ret = {.op = XR_LIR_RET, .type = XR_LIR_UNIT, .operands = &value, .operand_count = 1};
    CHECK(xr_lir_set_terminator(module, block, &ret) == XR_LIR_OK);
    CHECK(xr_lir_module_finish(module, NULL) == XR_LIR_OK);
    XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrLirCRequest request = {module, entry, resources, "publication-fault"};
    XrLirCOutput output = {0};
    XrLirCDiagnostic diagnostic;
    if (argc == 2) {
        corrupt_text = true;
        (void) xr_lir_emit_c(&request, &output, &diagnostic);
        puts("UNEXPECTED_PUBLICATION");
        return 1;
    }
    for (unsigned point = 0; point < 3; ++point) {
        fail_workspace = point == 0;
        fail_text = point == 1;
        fail_divisors = point == 2;
        injected = false;
        CHECK(xr_lir_emit_c(&request, &output, &diagnostic) == XR_LIR_C_OUT_OF_MEMORY);
        CHECK(injected && !output.bytes && !output.length && !output.entry_result);
        CHECK(diagnostic.status == XR_LIR_C_OUT_OF_MEMORY);
        fail_workspace = fail_text = fail_divisors = false;
        CHECK(xr_lir_emit_c(&request, &output, &diagnostic) == XR_LIR_C_OK);
        xr_lir_c_output_free(&output);
    }
    bool cancelled = true;
    CHECK(xr_compile_resources_begin_cancel(resources, cancel_now, &cancelled) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_lir_emit_c(&request, &output, &diagnostic) == XR_LIR_C_CANCELLED);
    CHECK(!output.bytes && !output.length);
    CHECK(xr_compile_resources_end_cancel(resources, cancel_now, &cancelled) == XR_COMPILE_RESOURCE_CANCELLED);
    cancelled = false;
    cancel_at_publication = &cancelled;
    CHECK(xr_compile_resources_begin_cancel(resources, cancel_now, &cancelled) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_lir_emit_c(&request, &output, &diagnostic) == XR_LIR_C_CANCELLED);
    CHECK(!output.bytes && !output.length && diagnostic.status == XR_LIR_C_CANCELLED);
    CHECK(xr_compile_resources_end_cancel(resources, cancel_now, &cancelled) == XR_COMPILE_RESOURCE_CANCELLED);
    cancel_at_publication = NULL;
    xr_lir_module_destroy(module);
    xr_compile_resources_release(resources);
    puts("representative C workspace/text failures and cancellation passed");
    return 0;
}
