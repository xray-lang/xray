/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_bool.c - Source bool tasks retain repeatable owned outcomes
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_task.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"

typedef struct BoolOracle { const char *name, *entry; bool value, escaped; } BoolOracle;
static const BoolOracle bool_oracles[] = {
    {"direct", "main", true, false}, {"generic", "main", false, false}, {"escape", "handle", true, true}};

static XrXirProgram *bool_program(const BoolOracle *oracle, uint32_t *entry) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/%s.xr", XR_TASK_BOOL_FIXTURES, oracle->name) > 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_TASK_BOOL_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult source = {0}; XrXirSourceDiagnostic source_diagnostic = {0}; char *failure = NULL;
    XrXirDiagnostic diagnostic = {0}; XrXirCheckedPacket packet = {0};
    XrXirArtifact *read = NULL, *closed = NULL, *lowered = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &source, &source_diagnostic, &failure);
    xr_compile_session_free(session);
    if (status != XR_XIR_OK) fprintf(stderr, "%s check=%u %s\n", oracle->name, status, source_diagnostic.message);
    CHECK(status == XR_XIR_OK && source.checked && !failure);
    CHECK(xr_xir_compile_checked_write(source.checked, &packet, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &read, &diagnostic) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(read, &closed, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_artifact_verify(closed, &diagnostic) == XR_XIR_OK);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    status = xr_xir_compile_lower(closed, &target, &lowered, &diagnostic);
    xr_xir_compile_artifact_free(closed);
    if (status != XR_XIR_OK) {
        fprintf(stderr, "%s lower=%u function=%u instruction=%u\n", oracle->name,
            status, diagnostic.function, diagnostic.instruction);
        CHECK(!lowered); return NULL;
    }
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered); unsigned matches = 0;
    size_t length = strlen(oracle->entry);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const XrXirFunctionIdentity *identity = &module->declarations->functions[f];
        if (function->name_length == length && !memcmp(function->name, oracle->entry, length) && identity->exported &&
            identity->module == module->declarations->root_module && !identity->nominal_owner) {
            CHECK(!function->parameter_count); *entry = f; ++matches;
        }
    }
    CHECK(matches == 1);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    return program;
}

static void bool_value(const XrXirValue *value, bool expected) {
    CHECK(value->type == XR_XIR_BOOL && !value->reserved && value->payload == (expected ? 1 : 0));
}
static bool bool_execute(const BoolOracle *oracle) {
    uint32_t entry = UINT32_MAX; XrXirProgram *program = bool_program(oracle, &entry);
    if (!program) return false;
    XrXirInstance *instances[2] = {NULL, NULL}; XrXirValue values[2] = {{0}};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstance *instance = instances[i];
        CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = {0}; unsigned steps = 0;
        do { result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++steps < 4096); }
        while (result.outcome.status == XR_XIR_CALL_READY);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance, &values[i]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    }
    if (oracle->escaped) CHECK(values[0].payload != values[1].payload);
    for (unsigned i = 0; i < 2; ++i) {
        if (oracle->escaped) {
            XrXirValue copy = {0}; CHECK(xr_xir_value_copy(&values[i], &copy) == XR_XIR_VALUE_OK);
            CHECK(copy.payload == values[i].payload && copy.type == values[i].type);
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                XrXirCallResult outcome = {0};
                CHECK(xr_xir_task_copy_outcome(repeat ? &copy : &values[i], &outcome) == XR_XIR_CALL_RETURNED);
                bool_value(&outcome.value, oracle->value); xr_xir_call_result_drop(&outcome);
            }
            xr_xir_value_drop(&copy);
        } else bool_value(&values[i], oracle->value);
        xr_xir_value_drop(&values[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
    printf("Task<bool> %s: two Instances, fixed=%u repeated owned result, escape=%u, runtime physical=0/0\n",
        oracle->name, oracle->value ? 1u : 0u, oracle->escaped ? 1u : 0u);
    return true;
}
static void bool_reject(const char *name) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/rejected/%s.xr", XR_TASK_BOOL_FIXTURES, name) > 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_TASK_BOOL_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, &failure);
    fprintf(stderr, "Task<bool> reject %s=%u: %s\n", name, status, diagnostic.message);
    CHECK(status == XR_XIR_BAD_TYPE && !result.checked && !result.snapshot);
    xr_compile_resources_free(failure); xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); bool complete = true;
    printf("Task<bool> storage: Executor=%zu Task=%zu Call=%zu Frame=%zu state_offset=%zu\n",
        sizeof(XrXirTaskExecutor), sizeof(XirTask), sizeof(XrXirCall), sizeof(CallFrame), (size_t)state_offset());
    for (size_t i = 0; i < sizeof(bool_oracles) / sizeof(*bool_oracles); ++i)
        if (!bool_execute(&bool_oracles[i])) complete = false;
    bool_reject("generic_missing"); bool_reject("mutable");
    bool_reject("construct"); bool_reject("invariance");
    effects_source_owners_free(); CHECK(complete); return 0;
}
