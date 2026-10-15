/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_effects_callable_bounds.c - Preserve variable inputs and erase fixed promises
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_types.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "callable bounds %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_effect_execution_compile.h"
#include "xir_source_manifest_fixture.h"
#include "xir_source_callable_bounds.h"

/* The original six declarations and all three source files remain intact.
 * Neither packet decoding nor these observations may consult a live snapshot. */
static void source_original_callable_bounds(XrXirSourceRequest *request) {
    const SourceTestDeclaration declarations[] = {
        {"dynamic", NULL, "f", true}, {"pure", NULL, NULL, true},
        {"weaken", NULL, "f", false}, {"literalDefault", NULL, "f", false},
        {"namedDefault", NULL, "f", false}, {"effectDefault", NULL, "f", false}};
    source_manifest_write(request, "root.xr", declarations, 6);
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    CHECK(effect_execution_check(request, &result, &diagnostic) == XR_XIR_OK);
    SourceCallableWeakenings counts = source_callable_weakenings(result.checked);
    CHECK(!counts.suspend && counts.identity > 0 && counts.root > 0);
    source_callable_promise_bindings(result.checked);
    const XrXirCompileContext context = *xr_xir_compile_artifact_context(result.checked);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result); source_manifest_raw(request, "");
    XrXirArtifact *read = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &read, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    SourceCallableWeakenings reloaded = source_callable_weakenings(read);
    CHECK(reloaded.identity == counts.identity && reloaded.root == counts.root && reloaded.suspend == counts.suspend);
    source_callable_promise_bindings(read);
    printf("original callable bounds identity=%u root=%u suspend=%u variableEdges=2, SourceDead packetDead\n",
        counts.identity, counts.root, counts.suspend);
    xr_xir_compile_artifact_free(read); effects_source_owners_free();
}
static void source_fixed_callable_inspect(const XrXirArtifact *checked) {
    SourceCallableWeakenings counts = source_callable_weakenings(checked);
    CHECK(counts.suspend == 1);
    const XrXirModule *module = xr_xir_compile_artifact_module(checked);
    uint32_t found = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *body = &module->functions[f];
        if (body->name_length != 5 || memcmp(body->name, "fixed", 5)) continue;
        CHECK(!found++ && body->parameter_count == 1);
        const XrXirTypeNode *parameter = xr_xir_callable_signature(module->types, body->parameters[0]);
        CHECK(parameter && parameter->flags == (XR_XIR_CALLABLE_ROOT_UNRESOLVED | XR_XIR_CALLABLE_NO_SUSPEND));
        uint32_t erased = 0;
        for (uint32_t i = 0; i < body->instruction_count; ++i) {
            const XrXirInstruction *op = &body->instructions[i];
            if (op->op != XR_XIR_FUNCTION_WEAKEN || op->args[0] != 0) continue;
            const XrXirTypeNode *bound = xr_xir_callable_signature(module->types, op->type);
            CHECK(bound && bound->flags == XR_XIR_CALLABLE_ROOT_UNRESOLVED); ++erased;
        }
        CHECK(erased == 1);
    }
    CHECK(found == 1);
}
static void source_fixed_callable_program(XrXirArtifact *lowered) {
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    uint32_t entry = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f)
        if (module->functions[f].name_length == 3 && !memcmp(module->functions[f].name, "run", 3)) entry = f;
    CHECK(entry != UINT32_MAX);
    XrXirCSource source = {0};
    CHECK(xr_xir_compile_emit_c(lowered, "fixed_callable", 4194304, &source) == XR_XIR_OK);
    CHECK(source.length && !strstr(source.text, "({") && !strstr(source.text, "xr_xir_vm"));
    xr_xir_compile_c_source_free(&source);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    XrXirInstance *instance = NULL; XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    CHECK(value.type == XR_XIR_I64 && value.payload == 7); xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    CHECK(!runtime_live && !runtime_bytes);
}
static void source_fixed_callable_bounds(XrXirSourceRequest *request, SourceTestFiles *files) {
    const char text[] = "fn pure()->i64{return 7}\n"
        "fn invoke(f:fn()->i64)->i64{return f()}\n"
        "fn fixed(f:fn()->i64)->i64{const erased:fn()->i64=f;return invoke(erased)}\n"
        "export fn run()->i64{return fixed(pure)}\n";
    FILE *file = fopen(request->entry_path, "wb"); CHECK(file);
    CHECK(fwrite(text, 1, sizeof(text) - 1, file) == sizeof(text) - 1 && fclose(file) == 0);
    const SourceTestDeclaration declarations[] = {{"fixed", NULL, "f", false}, {"pure", NULL, NULL, true}};
    source_manifest_write(request, "root.xr", declarations, 2);
    XrXirSourceResult result = {0};
    CHECK(effect_execution_check(request, &result, NULL) == XR_XIR_OK);
    source_fixed_callable_inspect(result.checked);
    const XrXirCompileContext context = *xr_xir_compile_artifact_context(result.checked);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result); source_files_close(files);
    XrXirArtifact *read = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, packet.bytes, packet.length, &read, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xCC, packet.length); xr_xir_compile_checked_packet_free(&packet);
    source_fixed_callable_inspect(read);
    CHECK(xr_xir_compile_specialize(read, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(read);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed); source_fixed_callable_program(lowered);
    effects_source_owners_free();
    puts("fixed callable promise erased once, Source/files/packetDead before lower, CGen/VM=7 physical0/0");
}
int main(void) {
    SourceTestFiles files = {0}; source_files_open(&files);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, files.root};
    XrXirSourceRequest request = {NULL, files.entry, &authority, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    source_original_callable_bounds(&request); source_fixed_callable_bounds(&request, &files);
    return 0;
}
