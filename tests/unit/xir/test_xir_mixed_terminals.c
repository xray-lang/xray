/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_terminals.c - Both providers preserve terminal payload and cleanup
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_panic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "mixed terminals %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec terminal_fixture_program;
XR_DATA const uint32_t terminal_fixture_functions[5];
typedef enum TerminalBackend {
    TERMINAL_MIXED_VM_ROOT, TERMINAL_MIXED_NATIVE_ROOT,
    TERMINAL_VM, TERMINAL_NATIVE
} TerminalBackend;
static const char *terminal_backend_names[] = {"0", "1", "vm", "native"};
typedef struct TerminalCode {
    XrXirProgram *vm;
    XrXirCallEntry *entries;
} TerminalCode;
static unsigned terminal_code_releases;
static void terminal_code_free(void *pointer) {
    TerminalCode *owner = pointer;
    xr_xir_compile_program_drop(owner->vm);
    xr_compile_resources_free(owner->entries); xr_compile_resources_free(owner);
    ++terminal_code_releases;
}
static XrXirProgram *terminal_program(TerminalBackend backend) {
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, terminal_fixture_program.proof.bytes,
        terminal_fixture_program.proof.length, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked, &terminal_fixture_program.target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == terminal_fixture_program.entry_count);
    TerminalCode *owner = NULL;
    CHECK(xr_compile_resources_calloc(context.resources, 1, sizeof(*owner), (void **)&owner) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources, module->function_count,
        sizeof(*owner->entries), (void **)&owner->entries) == XR_COMPILE_RESOURCE_OK);
    XrXirProgramProof proof = xr_xir_compile_program_proof(lowered);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &owner->vm) == XR_XIR_OK && !lowered);
    unsigned vm = 0, native = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        bool root = module->declarations->functions[f].module == module->declarations->root_module;
        bool use_native = backend == TERMINAL_NATIVE || (backend < TERMINAL_VM &&
            root == (backend == TERMINAL_MIXED_NATIVE_ROOT));
        CHECK(terminal_fixture_program.entries[f].resume != owner->vm->entries[f].resume);
        owner->entries[f] = use_native ? terminal_fixture_program.entries[f] : owner->vm->entries[f];
        if (use_native) ++native; else ++vm;
    }
    if (backend == TERMINAL_VM) CHECK(vm == module->function_count && !native);
    else if (backend == TERMINAL_NATIVE) CHECK(native == module->function_count && !vm);
    else CHECK(vm && native);
    XrXirProgramSpec spec = terminal_fixture_program;
    spec.entries = owner->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){owner,terminal_code_free};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context, &spec, &program) == XR_XIR_OK && program);
    return program;
}
typedef struct TerminalTrace { unsigned kind, count; } TerminalTrace;
static XrXirOutputStatus terminal_output(void *pointer, const XrXirOutputGroup *group) {
    TerminalTrace *trace = pointer;
    CHECK(trace->kind < 4 && trace->count < (trace->kind == 3 ? 4u : 2u));
    const int64_t crossed[] = {40,41,42,41};
    const int64_t expected = trace->kind == 3 ? crossed[trace->count] :
        (int64_t)(trace->kind + 1) * 10 + (trace->count ? 0 : 1);
    CHECK(group->count == 1 && group->line && group->stream == XR_XIR_STDOUT);
    CHECK(group->values[0].type == XR_XIR_I64 && group->values[0].payload == expected);
    ++trace->count;
    return XR_XIR_OUTPUT_OK;
}
static void terminal_text(const XrXirValue *value, const char *expected) {
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == strlen(expected) && !memcmp(bytes, expected, length));
}
static void terminal_drive(XrXirInstance *instance, unsigned kind, bool cancel,
    TerminalTrace *trace, XrXirCallResult *retained) {
    CHECK(xr_xir_instance_start(instance, terminal_fixture_functions[kind], NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult paused = xr_xir_instance_poll_bounded(instance, UINT64_MAX);
    CHECK(paused.outcome.status == XR_XIR_CALL_SUSPENDED && paused.epoch && paused.outcome.wake && !trace->count);
    CHECK(xr_xir_instance_resume(instance, paused.epoch + 1, paused.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_resume(instance, paused.epoch, paused.outcome.wake + 1) == XR_XIR_CALL_BAD_STATE);
    if (cancel) CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_CANCEL_REQUESTED);
    else {
        CHECK(xr_xir_instance_resume(instance, paused.epoch, paused.outcome.wake) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_resume(instance, paused.epoch, paused.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    }
    XrXirCallResult outcome = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome;
    const XrXirCallStatus expected[] = {XR_XIR_CALL_RETURNED,XR_XIR_CALL_THROWN,XR_XIR_CALL_DIVIDE_BY_ZERO};
    CHECK(outcome.status == (cancel ? XR_XIR_CALL_CANCELLED : expected[kind]));
    CHECK(trace->count == 2);
    CHECK(xr_xir_instance_resume(instance, paused.epoch, paused.outcome.wake) == XR_XIR_CALL_BAD_STATE);
    if (cancel) {
        CHECK(xr_xir_instance_cancel_current(instance) == XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED);
        CHECK(trace->count == 2);
        return;
    }
    CHECK(xr_xir_call_result_copy(&outcome, retained) == XR_XIR_VALUE_OK);
    XrXirValue value = {0};
    if (kind < 2) {
        XrXirValue occupied = {XR_XIR_I64,0,73}, original = occupied;
        CHECK(xr_xir_instance_take_result(instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!memcmp(&occupied, &original, sizeof(occupied)));
        CHECK(xr_xir_instance_take_result(instance, &value) == expected[kind]);
        xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_BAD_STATE && !value.type && !value.payload);
    } else {
        CHECK(outcome.panic.detail.code == 420 && outcome.value.type == XR_XIR_UNIT);
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_BAD_STATE && !value.type && !value.payload);
    }
}
static void terminal_retained(XrXirCallResult retained[3]) {
    terminal_text(&retained[0].value, "held");
    XrXirEnumBorrow error = {0};
    CHECK(xr_xir_enum_borrow(&retained[1].value, &error) == XR_XIR_VALUE_OK);
    CHECK(error.name.length == 13 && !memcmp(error.name.bytes, "TerminalFault", 13));
    CHECK(error.member.length == 6 && !memcmp(error.member.bytes, "Failed", 6) && error.field_count == 1);
    terminal_text(&error.fields[0], "held-error");
    CHECK(retained[2].status == XR_XIR_CALL_DIVIDE_BY_ZERO && retained[2].panic.detail.code == 420);
    char scratch[XR_XIR_PANIC_MESSAGE_CAPACITY]; XrErrorCoreMessageView message = {0};
    CHECK(xr_xir_panic_message_borrow(&retained[2].panic, scratch, sizeof(scratch), &message));
    CHECK(message.message_len == strlen("division by zero") &&
        !memcmp(message.message, "division by zero", message.message_len));
    for (unsigned i = 0; i < 3; ++i) xr_xir_call_result_drop(&retained[i]);
}
static void terminal_closure(TerminalBackend backend) {
    const unsigned released = terminal_code_releases;
    XrXirProgram *program = terminal_program(backend);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *origin = NULL, *peer = NULL;
    CHECK(xr_xir_instance_new(program, &config, &origin) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program, &config, &peer) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(origin, terminal_fixture_functions[4], NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(origin, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue function = {0}, alias = {0}, late_alias = {0};
    CHECK(xr_xir_instance_take_result(origin, &function) == XR_XIR_CALL_RETURNED);
    const XrXirFunctionBinding *binding = xr_xir_function_binding(&function);
    CHECK(binding && binding->capture_count == 1);
    terminal_text(&binding->captures[0], "captured");
    CHECK(xr_xir_value_copy(&function, &alias) == XR_XIR_VALUE_OK);
    const size_t blocks = runtime_live, bytes = runtime_bytes;
    CHECK(xr_xir_instance_start_function(peer, &function, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_state(peer) == XR_XIR_INSTANCE_NEW && runtime_live == blocks && runtime_bytes == bytes);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start_function(origin, i ? &alias : &function, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(origin, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue result = {0};
        CHECK(xr_xir_instance_take_result(origin, &result) == XR_XIR_CALL_RETURNED);
        terminal_text(&result, "captured"); xr_xir_value_drop(&result);
    }
    CHECK(xr_xir_instance_stop(origin) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start_function(origin, &function, NULL, 0) == XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_start_function(peer, &function, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_state(peer) == XR_XIR_INSTANCE_NEW);
    CHECK(xr_xir_instance_free(origin) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(peer) == XR_XIR_CALL_READY);
    CHECK(terminal_code_releases == released && runtime_live && runtime_bytes);
    CHECK(xr_xir_value_valid(&function) && xr_xir_value_valid(&alias));
    CHECK(xr_xir_value_copy(&alias, &late_alias) == XR_XIR_VALUE_OK);
    binding = xr_xir_function_binding(&late_alias);
    CHECK(binding && binding->capture_count == 1);
    terminal_text(&binding->captures[0], "captured");
    xr_xir_value_drop(&function); xr_xir_value_drop(&alias);
    CHECK(terminal_code_releases == released && xr_xir_value_valid(&late_alias));
    binding = xr_xir_function_binding(&late_alias);
    CHECK(binding && binding->capture_count == 1);
    terminal_text(&binding->captures[0], "captured");
    xr_xir_value_drop(&late_alias);
    CHECK(terminal_code_releases == released + 1 && !runtime_live && !runtime_bytes);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    unsigned selected = 0;
    while (selected < 4 && strcmp(argv[1], terminal_backend_names[selected])) ++selected;
    CHECK(selected < 4);
    const TerminalBackend backend = (TerminalBackend)selected;
    CHECK(!runtime_live && !runtime_bytes);
    XrXirProgram *program = terminal_program(backend);
    XrXirInstance *instances[7] = {0}; TerminalTrace traces[7] = {0};
    XrXirCallResult retained[3] = {{0}};
    for (unsigned i = 0; i < 7; ++i) {
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        traces[i].kind = i == 6 ? 3 : i % 3;
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,terminal_output,&traces[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    CHECK(!terminal_code_releases);
    for (unsigned i = 0; i < 6; ++i) {
        terminal_drive(instances[i], i % 3, i >= 3, &traces[i], &retained[i % 3]);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
        CHECK(!terminal_code_releases);
        for (unsigned peer = i + 1; peer < 7; ++peer) CHECK(!traces[peer].count);
    }
    CHECK(xr_xir_instance_start(instances[6], terminal_fixture_functions[3], NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instances[6], UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    CHECK(traces[6].count == 4);
    XrXirValue crossed = {0};
    CHECK(xr_xir_instance_take_result(instances[6], &crossed) == XR_XIR_CALL_RETURNED);
    CHECK(crossed.type == XR_XIR_I64 && crossed.payload == 42);
    xr_xir_value_drop(&crossed);
    CHECK(xr_xir_instance_free(instances[6]) == XR_XIR_CALL_READY && terminal_code_releases == 1);
    CHECK(runtime_live && runtime_bytes);
    terminal_retained(retained);
    CHECK(!runtime_live && !runtime_bytes);
    terminal_closure(backend);
    effects_source_owners_free();
    printf("Terminal paths backend=%s return/throw/panic and three cancels, inner/outer cleanup, callback trace=40/41/42/41 result42, late events rejected, retained payloads, closure captured/read/copied after instances, revoked/cross-instance rejected, final code lease released once, physical=0/0\n", terminal_backend_names[backend]);
    return 0;
}
