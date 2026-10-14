/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_text.c - Explicit source bytes through the owned product pipeline
 *
 * KEY CONCEPT:
 *   Text, authority and parser owners die before product queries and execution.
 */
#include "program/xr_xir_source_product.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_output.h"
#include "toolchain/xcompiler_session.h"
#include "execution/xr_xir_host_execution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_vm.c"
#include "program/xr_xir_source_product.c"

typedef struct TextWorld {
    XrCompileResources *resources;
    XrCompilerSession *session;
    XrXirCompileContext context;
    XrModuleIdentityAuthority authority;
    XrXirSourceProductRequest request;
} TextWorld;
static void world_open(TextWorld *w, uint64_t work) {
    memset(w, 0, sizeof(*w));
    XrCompileResourceLimits limits = {UINT64_C(64) << 20, UINT64_C(8) << 20, work};
    CHECK(xr_compile_resources_new(&limits, &w->resources) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(w->resources, &w->session) == XR_COMPILER_SESSION_OK);
    w->context = (XrXirCompileContext){w->resources, xr_xir_compile_default_limits()};
    w->authority = (XrModuleIdentityAuthority){.kind = XR_MODULE_IDENTITY_MEMORY,
        .namespace_id = "source-product-text-owned"};
    w->request = (XrXirSourceProductRequest){{w->session, NULL, &w->authority,
        &w->context, NULL, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
}
static void world_close(TextWorld *w) {
    xr_compile_session_free(w->session);
    xr_compile_resources_release(w->resources);
    memset(w, 0, sizeof(*w));
}
typedef struct Bytes { char data[32]; size_t length; } Bytes;
static XrXirOutputStatus write_bytes(void *context, XrXirOutputStream stream,
                                     const char *bytes, size_t length) {
    Bytes *out = context;
    if (stream != XR_XIR_STDOUT || length > sizeof(out->data) - out->length)
        return XR_XIR_OUTPUT_ERROR;
    memcpy(out->data + out->length, bytes, length); out->length += length;
    return XR_XIR_OUTPUT_OK;
}
static void text_owned_execution(void) {
    TextWorld w; world_open(&w, UINT64_C(128000000));
    const char original[] = "print(\"A\\0B\")\n";
    char bytes[sizeof(original) - 1]; memcpy(bytes, original, sizeof(bytes));
    XrXirSourceText text = {NULL, bytes, sizeof(bytes)};
    XrXirSourceProduct *product = NULL;
    CHECK(xr_xir_compile_source_product_build_text(&w.request, &text, &product, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_public_admit(product, NULL) == XR_XIR_OK);
    memset(bytes, '!', sizeof(bytes));
    world_close(&w);
    CHECK(xr_xir_compile_source_product_view(product)->complete);
    CHECK(xr_xir_compile_source_product_verify(product, 1048576, NULL) == XR_XIR_OK);
    const XrXirSourceProductFacts *facts = xr_xir_compile_source_product_facts(product);
    CHECK(facts && facts->module_count == 1); uint32_t entry = facts->entry;
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product, &program) == XR_XIR_OK);
    CHECK(xr_xir_compile_source_product_vm_take(product, &program) == XR_XIR_BAD_STRUCTURE);
    xr_xir_compile_source_product_free(product);
    Bytes out = {{0}, 0};
    XrXirOutputSink sink = {XR_XIR_CALL_ABI_VERSION, 0, write_bytes, &out, sizeof(out.data)};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, xr_xir_output_render, &sink};
    XrXirHostExecutionRequest run = {program, &config, entry, NULL, 0};
    XrXirCallResult result = {0};
    CHECK(xr_xir_host_execute(&run, &result) == XR_XIR_CALL_RETURNED);
    CHECK(result.value.type == XR_XIR_I64 && result.value.payload == 0);
    CHECK(out.length == 4 && !memcmp(out.data, "A\0B\n", 4));
    xr_xir_compile_program_drop(program); xr_xir_call_result_drop(&result);
    CHECK(!runtime_live && !runtime_bytes);
}
static void rejects_and_diagnostics(void) {
    TextWorld w; world_open(&w, UINT64_C(128000000));
    XrXirSourceProduct *out = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_source_product_build_text(&w.request, NULL, &out, &diagnostic) == XR_XIR_BAD_STRUCTURE);
    CHECK(!out && diagnostic.stage == XR_XIR_SOURCE_PRODUCT_INPUT);
    const char source[] = "var value = missing_symbol\n";
    XrXirSourceText text = {NULL, source, sizeof(source) - 1};
    CHECK(xr_xir_compile_source_product_build_text(&w.request, &text, &out, &diagnostic) != XR_XIR_OK);
    CHECK(!out && diagnostic.stage == XR_XIR_SOURCE_PRODUCT_CHECK);
    CHECK(diagnostic.source.line == 1 && diagnostic.source.column == 13);
    CHECK(strstr(diagnostic.source.message, "initialized value"));
    world_close(&w);
    CHECK(diagnostic.source.line == 1 && diagnostic.source.column == 13);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    CHECK(!runtime_live && !runtime_bytes);
    world_open(&w, UINT64_C(128000000));
    const char nul[] = {'p','r','i','n','t','(',')',0,'\n'};
    text = (XrXirSourceText){NULL, nul, sizeof(nul)};
    CHECK(xr_xir_compile_source_product_build_text(&w.request, &text, &out, &diagnostic) != XR_XIR_OK && !out);
    xr_xir_compile_source_product_diagnostic_free(&diagnostic); world_close(&w);
    CHECK(!runtime_live && !runtime_bytes);
}
static void complete_compiler_failures(void) {
    size_t count = 0;
    for (size_t point = 0; !point || point <= count; ++point) {
        runtime_attempts = 0; runtime_fail_at = point ? point - 1 : SIZE_MAX;
        XrCompileResourceLimits limits = {UINT64_C(64) << 20, UINT64_C(8) << 20, UINT64_C(128000000)};
        TextWorld w = {0}; XrXirSourceProduct *out = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
        XrCompileResourceStatus created = xr_compile_resources_new(&limits, &w.resources);
        XrCompilerSessionStatus opened = created == XR_COMPILE_RESOURCE_OK ?
            xr_compile_session_new(w.resources, &w.session) : XR_COMPILER_SESSION_OUT_OF_MEMORY;
        XrXirStatus status = XR_XIR_OUT_OF_MEMORY;
        if (opened == XR_COMPILER_SESSION_OK) {
            w.context = (XrXirCompileContext){w.resources, xr_xir_compile_default_limits()};
            w.authority = (XrModuleIdentityAuthority){.kind=XR_MODULE_IDENTITY_MEMORY,.namespace_id="source-text-faults"};
            w.request = (XrXirSourceProductRequest){{w.session,NULL,&w.authority,&w.context,NULL,NULL,XR_XIR_PROGRAM,NULL},
                {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
            const char source[] = "const n:i64 = 42\n";
            XrXirSourceText text = {NULL,source,sizeof(source)-1};
            status = xr_xir_compile_source_product_build_text(&w.request,&text,&out,&diagnostic);
        }
        if (!point) { CHECK(status == XR_XIR_OK); count = runtime_attempts; CHECK(count); }
        else { CHECK(status == XR_XIR_OUT_OF_MEMORY && !out && runtime_attempts > runtime_fail_at); }
        runtime_fail_at = SIZE_MAX; world_close(&w);
        xr_xir_compile_source_product_free(out); xr_xir_compile_source_product_diagnostic_free(&diagnostic);
        CHECK(!runtime_live && !runtime_bytes);
    }
    printf("text-product full compiler allocation faults=%zu; original ledger and physical zero\n",count);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--compiler-faults")) complete_compiler_failures();
    else { CHECK(argc == 1); text_owned_execution(); rejects_and_diagnostics(); }
    puts("owned explicit source product PASS"); return 0;
}

