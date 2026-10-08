/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_array_source_refusals.c - Array refusal diagnostic lifetime
 *
 * KEY CONCEPT:
 *   Original semantic failures leave empty outputs; occupied output guards preserve
 *   a real verified product. Owned diagnostics outlive parse sessions, input paths,
 *   the product and the caller's ledger reference.
 */
#include "base/xplatform.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "array_default_source_shape.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Actual Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22u && XR_XIR_CALL_ABI_VERSION == 28u &&
    XR_XIR_PROGRAM_ABI_VERSION == 29u, "Actual product identity");

typedef struct RejectionCase { const char *name, *message; XrXirStatus status; int column; } RejectionCase;
static const RejectionCase cases[] = {
    {"default_string", "no admitted default initializer", XR_XIR_BAD_TYPE, -1},
    {"default_bool", NULL, XR_XIR_BAD_TYPE, -1},
    {"read_push", "mutable", XR_XIR_BAD_TYPE, -1},
    {"move_copyable", "parameter contract", XR_XIR_BAD_TYPE, -1}
};

static void path_join(char output[2048], const char *root, const char *suffix) {
    int count = snprintf(output, 2048, "%s/%s", root, suffix);
    CHECK(count > 0 && count < 2048);
}

static bool same_path(const char *left, const char *right) {
    if (!left || !right) return false;
    while (*left && *right) {
        char a = *left++, b = *right++;
#if XR_OS_WINDOWS
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
#endif
        if (a != b) return false;
    }
    return !*left && !*right;
}

static void exact_diagnostic(const RejectionCase *test, const char *root, const XrXirSourceProductDiagnostic *error) {
    char relative[128], file[2048]; int count = snprintf(relative, sizeof(relative), "%s/main.xr", test->name);
    CHECK(count > 0 && (size_t)count < sizeof(relative)); path_join(file, root, relative);
    CHECK(error->stage == XR_XIR_SOURCE_PRODUCT_CHECK && error->status == test->status &&
        error->source.status == test->status && error->source.module == 0 && error->source.line == 1);
    if (test->column >= 0) CHECK(error->source.column > 0 && error->source.column == test->column);
    CHECK(error->source.message[0] && same_path(error->source_path, file) && !error->snapshot);
    if (test->message) CHECK(strstr(error->source.message, test->message));
}

static void exact_product(const XrXirSourceProduct *product, const XrXirSourceProductFacts *facts,
    const XrXirCheckedPacket *golden) {
    CHECK(xr_xir_compile_source_product_verify(product, 16777216, NULL) == XR_XIR_OK);
    const XrXirSourceProductFacts *actual = xr_xir_compile_source_product_facts(product);
    CHECK(actual && actual->module_count == 1 && actual->function_count == 5 && actual->entry < 5 &&
        actual->target.architecture == XR_XIR_ARCH_X86_64 && actual->target.abi_version == XR_XIR_VALUE_ABI_VERSION);
    CHECK(!memcmp(actual, facts, sizeof(*facts)));
    XrXirSourceProductPacketView view = {0};
    CHECK(xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &view) == XR_XIR_OK);
    CHECK(view.length == golden->length && !memcmp(view.bytes, golden->bytes, view.length));
}

static XrXirSourceProduct *positive_owner(const XrXirCompileContext *context, const char *fixture_root) {
    char root[2048], file[2048]; path_join(root, fixture_root, "single_module"); path_join(file, root, "root.xr");
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrXirSourceProductRequest request = {{session, file, &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic error = {0};
    CHECK(xr_xir_compile_source_product_build(&request, &product, &error) == XR_XIR_OK && product);
    xr_xir_compile_source_product_diagnostic_free(&error); xr_compile_session_free(session);
    memset(root, 0xa5, sizeof(root)); memset(file, 0xa5, sizeof(file));
    memset(&request, 0xa5, sizeof(request)); memset(&authority, 0xa5, sizeof(authority));
    return product;
}

static XrXirSourceProductDiagnostic reject(const XrXirCompileContext *context, const char *fixture_root,
    const RejectionCase *test, XrXirSourceProduct *before) {
    char root[2048], relative[128], file[2048]; CHECK(strlen(fixture_root) < sizeof(root)); strcpy(root, fixture_root);
    int count = snprintf(relative, sizeof(relative), "%s/main.xr", test->name);
    CHECK(count > 0 && (size_t)count < sizeof(relative)); path_join(file, root, relative);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, root};
    XrXirSourceProductRequest request = {{session, file, &authority, context, NULL, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *output = before; XrXirSourceProductDiagnostic error = {0};
    XrCompileResourceStats stock = {0}, after = {0};
    CHECK(xr_compile_resources_stats(context->resources, &stock) == XR_COMPILE_RESOURCE_OK);
    size_t attempts = instance_compile_attempts;
    XrXirStatus status = xr_xir_compile_source_product_build(&request, &output, &error);
    printf("array-source-refusal diagnostic=%s actual=%u stage=%u module=%u line=%d column=%d\n",
        test->name, status, error.stage, error.source.module, error.source.line, error.source.column);
    CHECK(status == (before ? XR_XIR_BAD_STRUCTURE : test->status) && output == before);
    if (before) {
        CHECK(instance_compile_attempts == attempts && error.stage == XR_XIR_SOURCE_PRODUCT_INPUT &&
            error.status == XR_XIR_BAD_STRUCTURE && !error.source_path && !error.snapshot && !error.source.message[0]);
        CHECK(xr_compile_resources_stats(context->resources, &after) == XR_COMPILE_RESOURCE_OK && !memcmp(&stock, &after, sizeof(stock)));
    }
    xr_compile_session_free(session);
    memset(root, 0xa5, sizeof(root)); memset(relative, 0xa5, sizeof(relative)); memset(file, 0xa5, sizeof(file));
    memset(&request, 0xa5, sizeof(request)); memset(&authority, 0xa5, sizeof(authority));
    if (!before) exact_diagnostic(test, fixture_root, &error);
    return error;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    instance_compile_zero(); XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    XrCompileResourceLimits caps = {UINT64_C(67108864), UINT64_C(16777216), UINT64_C(128000000)};
    CHECK(xr_compile_resources_new(&caps, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrXirSourceProduct *product = positive_owner(&context, argv[1]);
    XrXirSourceProductFacts facts = *xr_xir_compile_source_product_facts(product);
    XrXirSourceProductPacketView packet = {0}; XrXirArtifact *checked = NULL; XrXirCheckedPacket golden = {0};
    CHECK(xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &golden, NULL) == XR_XIR_OK);
    exact_product(product, &facts, &golden);
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    CHECK(module && module->function_count == 5 && module->declarations && !module->declarations->slot_count);
    CHECK(array_default_source_shape(module) != UINT32_MAX);
    unsigned transitions = 0;
    for (unsigned c = 0; c < 4; ++c) for (unsigned occupied = 0; occupied < 2; ++occupied) {
        size_t live = instance_compile_live, bytes = instance_compile_bytes;
        XrXirSourceProductDiagnostic error = reject(&context, argv[1], &cases[c], occupied ? product : NULL);
        exact_product(product, &facts, &golden);
        xr_xir_compile_source_product_diagnostic_free(&error);
        CHECK(!error.source_path && !error.snapshot && !error.source.message[0]);
        xr_xir_compile_source_product_diagnostic_free(&error);
        CHECK(instance_compile_live == live && instance_compile_bytes == bytes);
        printf("array-source-refusal case=%s occupied=%u actual=%u session-dead=1 path-dead=1 owner-exact=1 physical-refund=1 result=PASS\n",
            cases[c].name, occupied, occupied ? XR_XIR_BAD_STRUCTURE : cases[c].status); ++transitions;
    }
    XrXirSourceProductDiagnostic escaped[4] = {0};
    for (unsigned c = 0; c < 4; ++c) {
        escaped[c] = reject(&context, argv[1], &cases[c], NULL); exact_product(product, &facts, &golden);
        printf("array-source-refusal semantic=%s status=%u stage=%u line=%d column=%d message=%s\n",
            cases[c].name, escaped[c].status, escaped[c].stage, escaped[c].source.line,
            escaped[c].source.column, escaped[c].source.message);
        for (unsigned occupied = 0; occupied < 2; ++occupied) {
            XrXirSourceProductDiagnostic saved = escaped[c];
            XrXirSourceProduct *output = occupied ? product : NULL, *before = output;
            XrCompileResourceStats stock = {0}, after = {0};
            CHECK(xr_compile_resources_stats(context.resources, &stock) == XR_COMPILE_RESOURCE_OK);
            size_t attempts = instance_compile_attempts;
            CHECK(xr_xir_compile_source_product_build(NULL, &output, &escaped[c]) == XR_XIR_BAD_STRUCTURE);
            CHECK(output == before && !memcmp(&saved, &escaped[c], sizeof(saved)) && instance_compile_attempts == attempts);
            CHECK(xr_compile_resources_stats(context.resources, &after) == XR_COMPILE_RESOURCE_OK && !memcmp(&stock, &after, sizeof(stock)));
            exact_diagnostic(&cases[c], argv[1], &escaped[c]); exact_product(product, &facts, &golden);
            printf("array-source-refusal diagnostic-guard=%s occupied=%u actual=1 old-diagnostic-exact=1 output-preserved=1 ledger-unchanged=1 result=PASS\n",
                cases[c].name, occupied);
        }
    }
    XrXirSourceProductDiagnostic saved_errors[4]; memcpy(saved_errors, escaped, sizeof(escaped));
    xr_xir_compile_checked_packet_free(&golden); xr_xir_compile_artifact_free(checked);
    xr_xir_compile_source_product_free(product); product = NULL;
    xr_compile_resources_release(context.resources); context.resources = NULL;
    CHECK(instance_compile_live > 0 && instance_compile_bytes > 0);
    for (unsigned c = 0; c < 4; ++c) {
        exact_diagnostic(&cases[c], argv[1], &escaped[c]);
        CHECK(!memcmp(&saved_errors[c], &escaped[c], sizeof(escaped[c])));
        printf("array-source-refusal escaped=%s product-dead=1 caller-ledger-dropped=1 exact-diagnostic=1 result=PASS\n", cases[c].name);
        xr_xir_compile_source_product_diagnostic_free(&escaped[c]);
    }
    instance_compile_zero(); instance_compile_report();
    printf("array-source-refusal transitions=%u diagnostic-guards=8 escaped=4 compiler-physical=0/0 result=PASS\n", transitions);
    return 0;
}
