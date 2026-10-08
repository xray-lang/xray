/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_module_ref_source.c - Source ref arguments and detached cell lowering
 *
 * KEY CONCEPT:
 *   Legal module loans retain positive expectations even when source admission fails.
 *   Local-cell controls do not replace module-place or interface-call duties.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "module_ref_source/cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Current public execution identity");

static bool named(const XrXirFunction *function, const char *name) {
    return function->name_length == strlen(name) && !memcmp(function->name, name, function->name_length);
}

static void inspect_local(const XrXirArtifact *checked, const char *name) {
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    CHECK(module && module->types && module->declarations && !module->declarations->slot_count);
    unsigned refs = 0, calls = 0, reads = 0, writes = 0, arrays = 0, places = 0;
    bool array = !strcmp(name, "local_array");
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (named(function, "bump") || named(function, "relay")) {
            CHECK(function->parameter_count == 1 && function->result == XR_XIR_UNIT);
            CHECK(xr_xir_type_is_cell(module->types, function->parameters[0]));
            XrXirType element = xr_xir_cell_element(module->types, function->parameters[0]);
            if (array) CHECK(xr_xir_array_element(module->types, element) == XR_XIR_I64);
            else CHECK(element == XR_XIR_I64);
            ++refs;
        }
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            XrXirOp op = function->instructions[i].op;
            calls += op == XR_XIR_CALL;
            reads += op == XR_XIR_CELL_READ;
            writes += op == XR_XIR_CELL_WRITE;
            arrays += op == XR_XIR_ARRAY_SET;
            places += op == XR_XIR_CELL_PLACE;
        }
    }
    CHECK(refs == 2 && calls >= 4 && (array ? arrays && places : reads && writes));
    printf("module-ref-shape case=%s refs=%u calls=%u cell-reads=%u cell-writes=%u array-writes=%u cell-places=%u slots=0\n",
        name, refs, calls, reads, writes, arrays, places);
}

static bool run_case(const ModuleRefSourceCase *test, const char *root, const char *stdlib) {
    char directory[2048], filename[2048];
    int count = snprintf(directory, sizeof(directory), "%s/%s", root, test->name);
    CHECK(count > 0 && (size_t)count < sizeof(directory));
    count = snprintf(filename, sizeof(filename), "%s/root.xr", directory);
    CHECK(count > 0 && (size_t)count < sizeof(filename));
    XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(33554432), UINT64_C(128000000)};
    XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline = {0}, stats = {0};
    CHECK(xr_compile_resources_stats(context.resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, directory};
    XrXirSourceProductRequest request = {
        {session, filename, &authority, &context, stdlib, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL;
    XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_product_build(&request, &product, &diagnostic);
    printf("module-ref-source case=%s expected=%u actual=%u stage=%u source-status=%u module=%u line=%d column=%d result=%s\n",
        test->name, test->expected, status, diagnostic.stage, diagnostic.source.status,
        diagnostic.source.module, diagnostic.source.line, diagnostic.source.column, status == test->expected ? "PASS" : "FAIL");
    if (status != XR_XIR_OK) {
        CHECK(!product && diagnostic.source.message[0]);
        printf("module-ref-diagnostic case=%s message=%s\n", test->name, diagnostic.source.message);
    } else {
        CHECK(product);
        XrXirSourceProductPacketView packet = {0}; XrXirArtifact *checked = NULL, *lowered = NULL;
        CHECK(xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
        xr_compile_session_free(session); session = NULL;
        xr_xir_compile_source_product_free(product); product = NULL;
        CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
        if (!strncmp(test->name, "local_", 6)) inspect_local(checked, test->name);
        CHECK(xr_xir_compile_lower(checked, &request.target, &lowered, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);
        CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(lowered);
        printf("module-ref-detached case=%s Checked=PASS Lowered=PASS session-dead=1 product-dead=1\n", test->name);
    }
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_xir_compile_source_product_free(product); xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == baseline.live_bytes);
    xr_compile_resources_release(context.resources); instance_compile_zero();
    printf("module-ref-cleanup case=%s physical=0/0\n", test->name);
    return status == test->expected;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    unsigned mismatches = 0;
    for (size_t i = 0; i < sizeof(module_ref_source_cases)/sizeof(module_ref_source_cases[0]); ++i)
        if (!run_case(&module_ref_source_cases[i], argv[1], argv[2])) ++mismatches;
    instance_compile_report();
    printf("module-ref-source cases=9 mismatches=%u physical=0/0 result=%s\n", mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
