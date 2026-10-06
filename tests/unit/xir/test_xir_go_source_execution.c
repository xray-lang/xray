/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_go_source_execution.c - Public source Task outcomes after owner escape
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_error.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"

typedef struct SourceTaskOracle { const char *name; XrXirCallStatus status; unsigned kind; int64_t value; } SourceTaskOracle;
enum { TASK_ORACLE_I64, TASK_ORACLE_STRING, TASK_ORACLE_NULLABLE, TASK_ORACLE_ERROR, TASK_ORACLE_PANIC };
static const SourceTaskOracle source_task_oracles[] = {
    {"integer", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 42},
    {"string", XR_XIR_CALL_RETURNED, TASK_ORACLE_STRING, 0},
    {"generic", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 42},
    {"context_ordinary", XR_XIR_CALL_RUNTIME_PANIC, TASK_ORACLE_PANIC, 0},
    {"context_go", XR_XIR_CALL_RUNTIME_PANIC, TASK_ORACLE_PANIC, 0},
    {"context_existing", XR_XIR_CALL_RETURNED, TASK_ORACLE_NULLABLE, 42},
    {"context_spawn", XR_XIR_CALL_RUNTIME_PANIC, TASK_ORACLE_PANIC, 0},
    {"grouped", XR_XIR_CALL_RUNTIME_PANIC, TASK_ORACLE_PANIC, 0},
    {"import_default", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 42},
    {"const_state", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 42},
    {"local_storage", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 43},
    {"unknown_task", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 84},
    {"value_error", XR_XIR_CALL_THROWN, TASK_ORACLE_ERROR, 0},
    {"shadow_class", XR_XIR_CALL_RETURNED, TASK_ORACLE_I64, 42},
    {"context_match", XR_XIR_CALL_RUNTIME_PANIC, TASK_ORACLE_PANIC, 0},
    {"context_match_block", XR_XIR_CALL_RUNTIME_PANIC, TASK_ORACLE_PANIC, 0},
    {"context_match_existing", XR_XIR_CALL_RETURNED, TASK_ORACLE_NULLABLE, 42}
};
static XrXirProgram *source_task_program(const SourceTaskOracle *oracle, uint32_t *entry) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/%s.xr", XR_GO_EXECUTION_FIXTURES, oracle->name) > 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_GO_EXECUTION_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic source_diagnostic = {0};
    XrXirDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirArtifact *decoded = NULL, *closed = NULL, *lowered = NULL; XrXirCheckedPacket packet = {0};
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &source_diagnostic, &failure);
    xr_compile_session_free(session); session = NULL;
    if (status != XR_XIR_OK) fprintf(stderr, "%s check=%u %s\n", oracle->name, status, source_diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && !failure);
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, &diagnostic) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    status = xr_xir_compile_lower(closed, &target, &lowered, &diagnostic);
    xr_xir_compile_artifact_free(closed);
    if (status != XR_XIR_OK) fprintf(stderr, "%s lower=%u f=%u op=%u\n", oracle->name,
        status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    unsigned matches = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        const XrXirFunctionIdentity *identity = &module->declarations->functions[f];
        if (function->name_length == 4 && !memcmp(function->name, "main", 4) && identity->exported &&
            identity->module == module->declarations->root_module && !identity->nominal_owner) {
            CHECK(!function->parameter_count); *entry = f; ++matches;
        }
    }
    CHECK(matches == 1);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    return program;
}
static void source_task_assert(const SourceTaskOracle *oracle, const XrXirCallResult *outcome) {
    CHECK(outcome->status == oracle->status && xr_xir_call_result_valid(outcome));
    if (oracle->kind == TASK_ORACLE_I64) CHECK(outcome->value.type == XR_XIR_I64 && outcome->value.payload == oracle->value);
    else if (oracle->kind == TASK_ORACLE_STRING) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_string_view(&outcome->value, &bytes, &length) && length == 6 && !memcmp(bytes, "A\0BA\0B", 6));
    } else if (oracle->kind == TASK_ORACLE_NULLABLE) {
        bool some = false; const XrXirValue *payload = NULL;
        CHECK(xr_xir_nullable_view(&outcome->value, &some, &payload) && some && payload &&
            payload->type == XR_XIR_I64 && payload->payload == 42);
    } else if (oracle->kind == TASK_ORACLE_ERROR) {
        XrXirValue underlying = outcome->value;
        if (underlying.type == XR_XIR_ERROR) CHECK(xr_xir_error_borrow(&outcome->value, &underlying));
        XrXirEnumBorrow value = {0}; const char *bytes = NULL; size_t length = 0;
        CHECK(xr_xir_enum_borrow(&underlying, &value) == XR_XIR_VALUE_OK && value.name.length == 7 &&
            !memcmp(value.name.bytes, "Problem", 7) && value.member.length == 6 &&
            !memcmp(value.member.bytes, "Failed", 6) && value.field_count == 1);
        CHECK(xr_xir_string_view(&value.fields[0], &bytes, &length) && length == 5 && !memcmp(bytes, "owned", 5));
    } else {
        CHECK(!outcome->value.type && !outcome->value.reserved && !outcome->value.payload);
        CHECK(outcome->panic.detail.code == XR_XIR_PANIC_NULL_UNWRAP && !outcome->panic.detail.reserved &&
            !outcome->panic.detail.index && !outcome->panic.detail.length);
    }
}
static void source_task_execute(const SourceTaskOracle *oracle) {
    uint32_t entry = UINT32_MAX; XrXirProgram *program = source_task_program(oracle, &entry);
    for (unsigned pass = 0; pass < 2; ++pass) {
        XrXirInstanceConfig config; XrXirInstance *instance = NULL; XrXirCallResult owned = {0};
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = {0}; unsigned transitions = 0;
        do { result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++transitions < 32768); }
        while (result.outcome.status == XR_XIR_CALL_READY);
        if (result.outcome.status != oracle->status) fprintf(stderr, "%s actual=%u expected=%u\n",
            oracle->name, result.outcome.status, oracle->status);
        source_task_assert(oracle, &result.outcome);
        CHECK(xr_xir_call_result_copy(&result.outcome, &owned) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        if (pass == 1) xr_xir_compile_program_drop(program);
        source_task_assert(oracle, &owned); xr_xir_call_result_drop(&owned);
    }
    CHECK(!runtime_live && !runtime_bytes);
    printf("Source Task %s: real export main=%u, two Instances, status=%u, escaped outcome and runtime physical=0/0\n",
        oracle->name, entry, oracle->status);
}
#include "xir_go_source_runtime_cases.h"
static void source_task_runtime_measure(const SourceTaskOracle *oracle) {
    uint32_t entry = UINT32_MAX; XrXirProgram *program = source_task_program(oracle, &entry);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    CHECK(config.requested_value_limit == UINT64_C(67108864) &&
        config.requested_call_limit == UINT64_C(67108864) && config.work_limit == UINT64_C(128000000));
    size_t live = runtime_live, bytes = runtime_bytes;
    XrXirDomainBudgetStats measured = {0}; runtime_attempts = 0;
    CHECK(source_task_runtime_operation(program, entry, oracle, &config, &measured) == oracle->status);
    size_t sites = runtime_attempts;
    CHECK(sites && sites < 4096 && runtime_live == live && runtime_bytes == bytes);
    printf("{\"kind\":\"runtime_measure\",\"case\":\"%s\",\"status\":%u,\"sites\":%zu,"
        "\"requested_value\":%llu,\"requested_call\":%llu,\"work\":%llu,"
        "\"frame_size\":%zu,\"frame_align\":%zu,\"state_offset\":%zu,"
        "\"metadata_live\":%llu,\"call_live\":%llu,\"metadata_peak\":%llu,\"call_peak\":%llu}\n",
        oracle->name, oracle->status, sites,
        (unsigned long long)measured.requested_bytes, (unsigned long long)measured.requested_call_bytes,
        (unsigned long long)measured.work, sizeof(CallFrame), _Alignof(CallFrame),
        (size_t)state_offset(),
        (unsigned long long)measured.metadata_live, (unsigned long long)measured.call_live,
        (unsigned long long)measured.metadata_peak, (unsigned long long)measured.call_peak);
    xr_xir_compile_program_drop(program); CHECK(!runtime_live && !runtime_bytes);
    printf("Source Task runtime measure %s physical=0/0, complete original operation, no injections\n", oracle->name);
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0); CHECK(argc >= 1 && argc <= 3);
    bool faults = argc == 3 && !strcmp(argv[1], "--runtime-oom");
    bool axes = argc == 3 && !strcmp(argv[1], "--runtime-axes");
    bool measure = argc == 3 && !strcmp(argv[1], "--runtime-measure");
    CHECK(argc != 3 || faults || axes || measure);
    const char *selected = argc == 3 ? argv[2] : argc == 2 ? argv[1] : NULL;
    unsigned count = 0;
    for (size_t i = 0; i < sizeof(source_task_oracles) / sizeof(*source_task_oracles); ++i)
        if (!selected || !strcmp(selected, source_task_oracles[i].name)) {
            if (faults) source_task_runtime_faults(&source_task_oracles[i]);
            else if (axes) source_task_runtime_axes(&source_task_oracles[i]);
            else if (measure) source_task_runtime_measure(&source_task_oracles[i]);
            else source_task_execute(&source_task_oracles[i]);
            ++count;
        }
    CHECK(count); effects_source_owners_free(); return 0;
}
