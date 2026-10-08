/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_parent_source.c - Class parent admission and detached root-class controls
 *
 * KEY CONCEPT:
 *   Legal parent chains retain positive expectations even when source admission fails.
 *   Root-class controls cannot establish inheritance or parent-validation behavior.
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "parent_source/cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Current public execution identity");

static void inspect_root(const XrXirArtifact *checked, const ParentSourceCase *test) {
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    CHECK(module && module->types && module->types->nominals && module->declarations);
    const XrXirNominalTable *table = module->types->nominals;
    unsigned roots = 0, constructions = 0;
    for (uint32_t i = 0; i < module->types->count; ++i) {
        const XrXirTypeNode *node = &module->types->nodes[i];
        if (node->kind != XR_XIR_TYPE_NOMINAL) continue;
        uint32_t d = node->nominal.declaration; CHECK(d < table->count);
        XrXirLiteral name = table->declarations ? table->declarations[d].name : table->identities[d].name;
        if (name.length != 4 || memcmp(name.bytes, "Root", 4)) continue;
        uint32_t kind = table->declarations ? table->declarations[d].kind : table->identities[d].kind;
        uint32_t flags = table->declarations ? table->declarations[d].flags : table->identities[d].flags;
        CHECK(kind == XR_XIR_NOMINAL_CLASS && flags == test->final);
        CHECK(node->nominal.field_count == test->fields && !node->nominal.argument_count);
        if (test->fields) CHECK(node->nominal.fields[0] == XR_XIR_I64 && node->nominal.fields[1] == XR_XIR_STRING);
        ++roots;
    }
    for (uint32_t f = 0; f < module->function_count; ++f)
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i)
            constructions += module->functions[f].instructions[i].op == XR_XIR_CLASS_NEW;
    CHECK(roots == 1 && constructions > 0);
    printf("parent-shape case=%s roots=%u fields=%u final=%u constructions=%u\n",
        test->name, roots, test->fields, test->final, constructions);
}

static bool run_case(const ParentSourceCase *test, const char *root, const char *stdlib) {
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
    printf("parent-source case=%s expected=%u actual=%u stage=%u source-status=%u module=%u line=%d column=%d result=%s\n",
        test->name, test->expected, status, diagnostic.stage, diagnostic.source.status,
        diagnostic.source.module, diagnostic.source.line, diagnostic.source.column, status == test->expected ? "PASS" : "FAIL");
    if (status != XR_XIR_OK) {
        CHECK(!product && diagnostic.source.message[0]);
        printf("parent-diagnostic case=%s message=%s\n", test->name, diagnostic.source.message);
    } else {
        CHECK(product);
        XrXirSourceProductPacketView packet = {0}; XrXirArtifact *checked = NULL, *lowered = NULL;
        CHECK(xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet) == XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
        xr_compile_session_free(session); session = NULL;
        xr_xir_compile_source_product_free(product); product = NULL;
        CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
        if (test->control) inspect_root(checked, test);
        CHECK(xr_xir_compile_lower(checked, &request.target, &lowered, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(checked);
        CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
        xr_xir_compile_artifact_free(lowered);
        printf("parent-detached case=%s Checked=PASS Lowered=PASS session-dead=1 product-dead=1\n", test->name);
    }
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_xir_compile_source_product_free(product); xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == baseline.live_bytes);
    xr_compile_resources_release(context.resources); instance_compile_zero();
    printf("parent-cleanup case=%s physical=0/0\n", test->name);
    return status == test->expected;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    unsigned mismatches = 0;
    for (size_t i = 0; i < sizeof(parent_source_cases)/sizeof(parent_source_cases[0]); ++i)
        if (!run_case(&parent_source_cases[i], argv[1], argv[2])) ++mismatches;
    instance_compile_report();
    printf("parent-source cases=11 mismatches=%u physical=0/0 result=%s\n", mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
