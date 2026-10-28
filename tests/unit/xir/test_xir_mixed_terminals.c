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
#include "xir/xxir_effect_contract_internal.h"
#include "xir/xxir_program_packet_internal.h"
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
enum { TERMINAL_ROOT, TERMINAL_CALLEE, TERMINAL_CALLBACK, TERMINAL_ROLES };
typedef struct TerminalWitness {
    XrXirInstance *instance;
    unsigned steps[2][TERMINAL_ROLES], bodies[2][TERMINAL_ROLES], outputs[2][TERMINAL_ROLES];
} TerminalWitness;
static TerminalWitness terminal_witnesses[2];
static const XrXirCallEntry *terminal_selected[64];
static bool terminal_native[64];
static uint32_t terminal_roles[TERMINAL_ROLES], terminal_entry_roles[64];
static unsigned terminal_callbacks[2];
static TerminalWitness *terminal_witness(const XrXirCallView *view) {
    for (unsigned i = 0; i < 2; ++i)
        if (view->instance == terminal_witnesses[i].instance) return &terminal_witnesses[i];
    CHECK(false); return NULL;
}
static XrXirAction terminal_resume(XrXirCallView *view) {
    uint32_t entry = xr_xir_call_current_entry(view->activation);
    CHECK(entry < 64 && terminal_selected[entry]);
    unsigned provider = terminal_native[entry] ? 1u : 0u;
    ++terminal_callbacks[provider];
    const void *environment = view->environment;
    XrXirAction action = terminal_selected[entry]->resume(view);
    CHECK(view->environment == environment);
    for (unsigned role = 0; role < TERMINAL_ROLES; ++role) if (terminal_entry_roles[entry] == role) {
        TerminalWitness *witness = terminal_witness(view);
        ++witness->steps[provider][role];
        if (action.kind == XR_XIR_ACTION_RETURN) ++witness->bodies[provider][role];
        if (action.kind == XR_XIR_ACTION_OUTPUT) ++witness->outputs[provider][role];
    }
    return action;
}
static uint32_t terminal_source_identity(const XrXirModule *module, bool root, const char *name) {
    uint32_t found = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if ((module->declarations->functions[f].module == module->declarations->root_module) != root) continue;
        if (module->functions[f].name_length != strlen(name) || memcmp(module->functions[f].name, name, strlen(name))) continue;
        CHECK(found == UINT32_MAX); found = f;
    }
    CHECK(found != UINT32_MAX); return found;
}

static void terminal_code_free(void *pointer) {
    TerminalCode *owner = pointer;
    xr_xir_compile_program_drop(owner->vm);
    xr_compile_resources_free(owner->entries); xr_compile_resources_free(owner);
    ++terminal_code_releases;
}
static XrXirProgram *terminal_program(TerminalBackend backend) {
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(64)*1024*1024, UINT64_C(128000000));
    XrXirArtifact *checked = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(&context, terminal_fixture_program.packet.bytes,
        terminal_fixture_program.packet.length, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked, &terminal_fixture_program.target, NULL, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    CHECK(module->function_count == terminal_fixture_program.entry_count && module->function_count < 64);
    CHECK(module->provenance && module->provenance->source && module->provenance->origins &&
        module->provenance->count == module->function_count);
    const XrXirModule *definition = xr_xir_compile_artifact_module(module->provenance->source);
    terminal_roles[TERMINAL_ROOT] = terminal_source_identity(definition, true, "crossed");
    terminal_roles[TERMINAL_CALLEE] = terminal_source_identity(definition, false, "crossed");
    terminal_roles[TERMINAL_CALLBACK] = terminal_source_identity(definition, true, "callback");
    CHECK(terminal_roles[TERMINAL_ROOT] == terminal_fixture_functions[3]);
    CHECK(terminal_roles[TERMINAL_ROOT] != terminal_roles[TERMINAL_CALLEE] &&
        terminal_roles[TERMINAL_ROOT] != terminal_roles[TERMINAL_CALLBACK] &&
        terminal_roles[TERMINAL_CALLEE] != terminal_roles[TERMINAL_CALLBACK]);
    TerminalCode *owner = NULL;
    CHECK(xr_compile_resources_calloc(context.resources, 1, sizeof(*owner), (void **)&owner) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context.resources, module->function_count,
        sizeof(*owner->entries), (void **)&owner->entries) == XR_COMPILE_RESOURCE_OK);
    XrXirProgramPacket packet = xir_program_packet_borrow(lowered);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &owner->vm) == XR_XIR_OK && !lowered);
    unsigned vm = 0, native = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        bool root = module->declarations->functions[f].module == module->declarations->root_module;
        bool use_native = backend == TERMINAL_NATIVE || (backend < TERMINAL_VM &&
            root == (backend == TERMINAL_MIXED_NATIVE_ROOT));
        CHECK(terminal_fixture_program.entries[f].resume != owner->vm->entries[f].resume);
        terminal_entry_roles[f] = UINT32_MAX;
        CHECK(module->provenance->origins[f].function < definition->function_count);
        for (unsigned role = 0; role < TERMINAL_ROLES; ++role)
            if (module->provenance->origins[f].function == terminal_roles[role]) {
                terminal_entry_roles[f] = role;
                bool source_root = role != TERMINAL_CALLEE;
                CHECK(use_native == (backend == TERMINAL_NATIVE || (backend < TERMINAL_VM &&
                    source_root == (backend == TERMINAL_MIXED_NATIVE_ROOT))));
            }
        terminal_native[f] = use_native;
        terminal_selected[f] = use_native ? &terminal_fixture_program.entries[f] : &owner->vm->entries[f];
        owner->entries[f] = *terminal_selected[f];
        owner->entries[f].resume = terminal_resume;
        if (use_native) ++native; else ++vm;
    }
    if (backend == TERMINAL_VM) CHECK(vm == module->function_count && !native);
    else if (backend == TERMINAL_NATIVE) CHECK(native == module->function_count && !vm);
    else CHECK(vm && native);
    XrXirProgramSpec spec = terminal_fixture_program;
    spec.entries = owner->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.packet = packet; spec.code = (XrXirCodeLease){owner,terminal_code_free};
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
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start_function(origin, i ? &alias : &function, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(origin, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue result = {0};
        CHECK(xr_xir_instance_take_result(origin, &result) == XR_XIR_CALL_RETURNED);
        terminal_text(&result, "captured"); xr_xir_value_drop(&result);
    }
    CHECK(xr_xir_instance_stop(origin) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(origin) == XR_XIR_CALL_READY && terminal_code_releases == released);
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
static void terminal_crossed_pair(XrXirInstance *instances[2], TerminalTrace traces[2]) {
    for (unsigned i = 0; i < 2; ++i) terminal_witnesses[i].instance = instances[i];
    /* Returned actions count each named Source activation once, independently
     * of provider-specific resume granularity or the aggregate callback total. */
    const unsigned bodies[TERMINAL_ROLES] = {1, 1, 2};
    const unsigned outputs[TERMINAL_ROLES] = {0, 2, 2};
    for (unsigned round = 0; round < 2; ++round) {
        for (unsigned i = 0; i < 2; ++i) {
            traces[i].count = 0;
            CHECK(xr_xir_instance_start(instances[i], terminal_fixture_functions[3], NULL, 0) == XR_XIR_CALL_READY);
        }
        bool done[2] = {false, false}; unsigned polls = 0;
        while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) {
            if (done[i]) continue;
            TerminalTrace peer_trace = traces[1-i];
            TerminalWitness peer = terminal_witnesses[1-i];
            XrXirCallStatus status = xr_xir_instance_poll_bounded(instances[i], 1).outcome.status;
            CHECK(++polls < 4096);
            CHECK(!memcmp(&peer_trace, &traces[1-i], sizeof(peer_trace)));
            CHECK(!memcmp(&peer, &terminal_witnesses[1-i], sizeof(peer)));
            CHECK(status == XR_XIR_CALL_READY || status == XR_XIR_CALL_RETURNED);
            done[i] = status == XR_XIR_CALL_RETURNED;
        }
        for (unsigned i = 0; i < 2; ++i) {
            CHECK(traces[i].count == 4);
            for (unsigned role = 0; role < TERMINAL_ROLES; ++role) {
                unsigned selected = terminal_native[terminal_roles[role]] ? 1u : 0u;
                TerminalWitness *witness = &terminal_witnesses[i];
                fprintf(stderr, "Source identity Instance%u round%u role%u entry%u provider%u steps=%u/%u bodies=%u/%u outputs=%u/%u\n",
                    i, round + 1, role, terminal_roles[role], selected, witness->steps[0][role], witness->steps[1][role],
                    witness->bodies[0][role], witness->bodies[1][role], witness->outputs[0][role], witness->outputs[1][role]);
                CHECK(witness->steps[selected][role] >= round + 1 && !witness->steps[1u-selected][role]);
                CHECK(witness->bodies[selected][role] == bodies[role] * (round + 1));
                CHECK(witness->outputs[selected][role] == outputs[role] * (round + 1));
                CHECK(!witness->bodies[1u-selected][role] && !witness->outputs[1u-selected][role]);
            }
            XrXirValue crossed = {0};
            CHECK(xr_xir_instance_take_result(instances[i], &crossed) == XR_XIR_CALL_RETURNED);
            CHECK(crossed.type == XR_XIR_I64 && crossed.payload == 42);
            xr_xir_value_drop(&crossed);
        }
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    unsigned selected = 0;
    while (selected < 4 && strcmp(argv[1], terminal_backend_names[selected])) ++selected;
    CHECK(selected < 4);
    const TerminalBackend backend = (TerminalBackend)selected;
    CHECK(!runtime_live && !runtime_bytes);
    XrXirProgram *program = terminal_program(backend);
    XrXirInstance *instances[8] = {0}; TerminalTrace traces[8] = {0};
    XrXirCallResult retained[3] = {{0}};
    for (unsigned i = 0; i < 8; ++i) {
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        traces[i].kind = i >= 6 ? 3 : i % 3;
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,terminal_output,&traces[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    CHECK(!terminal_code_releases);
    for (unsigned i = 0; i < 6; ++i) {
        terminal_drive(instances[i], i % 3, i >= 3, &traces[i], &retained[i % 3]);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
        CHECK(!terminal_code_releases);
        for (unsigned peer = i + 1; peer < 8; ++peer) CHECK(!traces[peer].count);
    }
    terminal_crossed_pair(&instances[6], &traces[6]);
    CHECK(xr_xir_instance_free(instances[6]) == XR_XIR_CALL_READY && !terminal_code_releases);
    CHECK(xr_xir_instance_free(instances[7]) == XR_XIR_CALL_READY && terminal_code_releases == 1);
    memset(terminal_witnesses, 0, sizeof(terminal_witnesses));
    CHECK(runtime_live && runtime_bytes);
    terminal_retained(retained);
    CHECK(!runtime_live && !runtime_bytes);
    terminal_closure(backend);
    CHECK(backend < TERMINAL_VM ? terminal_callbacks[0] && terminal_callbacks[1] :
        backend == TERMINAL_VM ? terminal_callbacks[0] && !terminal_callbacks[1] :
        terminal_callbacks[1] && !terminal_callbacks[0]);
    effects_source_owners_free();
    printf("Terminal paths backend=%s return/throw/panic and three cancels, inner/outer cleanup, callback trace=40/41/42/41 result42, late events rejected, retained payloads, Source closure captured/read/copied after Instances, final code lease once, crossed pair two rounds bodies=1/1/2 per activation, peer unchanged, VM%u native%u callbacks, physical=0/0\n",
        terminal_backend_names[backend], terminal_callbacks[0], terminal_callbacks[1]);
    return 0;
}
