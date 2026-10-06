/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_go_source_product.c - Multi-module typed output and terminal Task escape
 */
#include "program/xr_xir_source_product.h"
#include "xir/xxir_task.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"

static XrXirOutputStatus source_product_output(void *context, const XrXirOutputGroup *group) {
    unsigned *outputs = context; CHECK(group && group->stream == XR_XIR_STDOUT && group->count == 1);
    const XrXirValue *value = group->values;
    if (*outputs == 0) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(value, &bytes, &length) && length == 3 && !memcmp(bytes, "A\0B", 3));
    } else CHECK(*outputs == 1 && value->type == XR_XIR_I64 && value->payload == 21 && !value->reserved);
    ++*outputs; return XR_XIR_OUTPUT_OK;
}
static XrXirValue source_product_run(XrXirInstance *instance, uint32_t function) {
    CHECK(xr_xir_instance_start(instance, function, NULL, 0) == XR_XIR_CALL_READY);
    for (unsigned step = 0; step < 32768; ++step) {
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, 1);
        if (result.outcome.status == XR_XIR_CALL_READY) continue;
        if (result.outcome.status != XR_XIR_CALL_RETURNED) fprintf(stderr, "product runtime status=%u\n", result.outcome.status);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        return value;
    }
    CHECK(false); return (XrXirValue){0};
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0); CHECK(argc == 1 || argc == 2);
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_GO_PRODUCT_FIXTURES};
    XrXirSourceProductRequest request = {{session, XR_GO_PRODUCT_FIXTURES "/main.xr", &authority,
        context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL}, {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL; XrXirSourceProductDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_source_product_build(&request, &product, &diagnostic);
    xr_compile_session_free(session); session = NULL;
    if (status != XR_XIR_OK) fprintf(stderr, "product compile stage=%u status=%u %s\n",
        diagnostic.stage, status, diagnostic.source.message);
    CHECK(status == XR_XIR_OK && product);
    CHECK(xr_xir_compile_source_product_verify(product, 4194304, NULL) == XR_XIR_OK);
    XrXirSourceProductFacts facts = *xr_xir_compile_source_product_facts(product);
    XrXirSourceProductPacketView packet = {0}; XrXirArtifact *closed = NULL;
    CHECK(xr_xir_compile_source_product_packet(product, XR_XIR_SOURCE_PRODUCT_CLOSED, &packet) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &closed, NULL) == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(closed);
    uint32_t handle = UINT32_MAX; unsigned matched = 0;
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->functions[f].name_length == 6 && !memcmp(module->functions[f].name, "handle", 6) &&
            module->declarations->functions[f].module == module->declarations->root_module &&
            module->declarations->functions[f].exported) { handle = f; ++matched; }
    CHECK(matched == 1); xr_xir_compile_artifact_free(closed);
    if (argc == 2) {
        XrXirCSource c = {0};
        CHECK(xr_xir_compile_source_product_emit(product, "go_product", 4194304, &c) == XR_XIR_OK);
        FILE *output = fopen(argv[1], "wb"); CHECK(output);
        CHECK(fwrite(c.text, 1, c.length, output) == c.length && fclose(output) == 0);
        xr_xir_compile_c_source_free(&c);
    }
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product, &program) == XR_XIR_OK);
    xr_xir_compile_source_product_free(product); product = NULL;
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    for (unsigned pass = 0; pass < 2; ++pass) {
        unsigned outputs = 0; XrXirInstanceConfig config; XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, source_product_output, &outputs};
        CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
        XrXirValue result = source_product_run(instance, facts.entry);
        CHECK(result.type == XR_XIR_I64 && !result.payload && outputs == 2); xr_xir_value_drop(&result);
        XrXirValue task = source_product_run(instance, handle), copied = {0};
        CHECK(xr_xir_value_copy(&task, &copied) == XR_XIR_VALUE_OK && outputs == 2);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        if (pass == 1) xr_xir_compile_program_drop(program);
        for (unsigned copy = 0; copy < 2; ++copy) {
            XrXirCallResult outcome = {0};
            CHECK(xr_xir_task_copy_outcome(copy ? &copied : &task, &outcome) == XR_XIR_CALL_RETURNED);
            CHECK(outcome.value.type == XR_XIR_I64 && outcome.value.payload == 21);
            xr_xir_call_result_drop(&outcome);
        }
        xr_xir_value_drop(&task); xr_xir_value_drop(&copied);
    }
    CHECK(!runtime_live && !runtime_bytes); effects_source_owners_free();
    printf("SourceProduct GO: logical modules=2, actual modules=%u, functions=%u, root=%u, handle=%u; two instances, typed A-NUL-B/21, escaped Task/copy terminal21, physical=0/0\n",
        facts.module_count, facts.function_count, facts.entry, handle);
    return 0;
}
