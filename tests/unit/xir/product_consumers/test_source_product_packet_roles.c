/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_packet_roles.c - Independent Checked roles and owned Unit tasks
 *
 * KEY CONCEPT:
 *   Unit outcomes, canonical entries and private initializers have distinct
 *   roles. A current packet never grants additional execution authority.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "source_product_packet_roles.h"

_Static_assert(XR_XIR_CHECKED_SCHEMA == 25 && XR_XIR_CHECKED_CONTRACT == 67, "current Checked identity");
_Static_assert(XR_XIR_VALUE_ABI_VERSION == 22 && XR_XIR_CALL_ABI_VERSION == 28 &&
    XR_XIR_PROGRAM_ABI_VERSION == 29, "current value, call and program identities");
_Static_assert(XR_XIR_CONST_INT == 2 && XR_XIR_PRINT == 24 && XR_XIR_THROW == 30 &&
    XR_XIR_RETURN == 33 && XR_XIR_INVOKE_ERROR == 102 && XR_XIR_GO == 146 &&
    XR_XIR_TASK_AWAIT == 147, "independent role opcode ordinals");

#ifdef XR_PACKET_ROLES_NATIVE
XR_DATA const XrXirProgramSpec packet_roles_program;
#endif

typedef struct RolePacket { const char *name; const uint8_t *bytes; size_t length; } RolePacket;
typedef struct RoleCodeOwner { XrXirArtifact *lowered; XrXirVmBinding bindings[5]; } RoleCodeOwner;
typedef struct RoleTrace { uint32_t begin, ready, output; } RoleTrace;
static unsigned role_code_releases;

static void role_zero(const XrXirValue *value) {
    CHECK(value && !value->type && !value->reserved && !value->payload);
}

static void role_shape(const XrXirModule *module) {
    CHECK(module && module->function_count == 5 && module->declarations && module->types);
    const XrXirDeclarations *d = module->declarations;
    CHECK(d->module_count == 1 && !d->slot_count && !d->literal_count && d->root_module == 0);
    CHECK(d->entry_function == 1 && d->modules[0].initializer == 0);
    CHECK(module->functions[0].result == XR_XIR_UNIT && !d->functions[0].exported);
    CHECK(module->functions[1].result == XR_XIR_I64 && !d->functions[1].exported);
    CHECK(module->functions[2].result == XR_XIR_UNIT && d->functions[2].exported);
    CHECK(module->functions[3].result == XR_XIR_UNIT && !d->functions[3].exported);
    CHECK(module->functions[4].result == (XrXirType)256 && d->functions[4].exported);
    CHECK(module->types->count == 1 && module->types->nodes[0].kind == XR_XIR_TYPE_TASK &&
        module->types->nodes[0].element == XR_XIR_UNIT && !module->types->nodes[0].parameter_span);
    const XrXirFunction *root = &module->functions[2];
    CHECK(root->block_count == 3 && root->instructions[1].op == XR_XIR_TASK_AWAIT);
    CHECK(root->instructions[root->blocks[1].first].op == XR_XIR_RETURN);
    CHECK(root->instructions[root->blocks[2].first].op == XR_XIR_INVOKE_ERROR);
    for (uint32_t f = 0; f < 5; ++f) {
        CHECK(!module->functions[f].parameter_count && !d->functions[f].test_role);
        CHECK(!d->functions[f].nominal_owner && !d->functions[f].cleanup_owner);
    }
}

static void role_packet_accept(const XrXirCompileContext *context, const RolePacket *input) {
    XrXirArtifact *checked = NULL;
    XrXirCheckedPacket written = {0};
    XrCompileResourceStats before = {0}, after = {0};
    CHECK(xr_compile_resources_stats(context->resources, &before) == XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_checked_read(context, input->bytes, input->length, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &written, NULL) == XR_XIR_OK);
    CHECK(written.length == input->length && !memcmp(written.bytes, input->bytes, input->length));
    if (!strcmp(input->name, "five_roles")) role_shape(xr_xir_compile_artifact_module(checked));
    xr_xir_compile_checked_packet_free(&written);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_compile_resources_stats(context->resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes == before.live_bytes);
    printf("current67 role=%s complete-reader-writer=1 bytes=%zu live-refund=1\n", input->name, input->length);
}

static void role_packet_reject(const XrXirCompileContext *context, const RolePacket *input,
    XrXirStatus expected, bool early) {
    for (unsigned occupied = 0; occupied < 2; ++occupied) {
        XrXirArtifact *out = occupied ? (XrXirArtifact *)(uintptr_t)1 : NULL;
        XrXirArtifact *before_out = out;
        XrCompileResourceStats before = {0}, after = {0};
        CHECK(xr_compile_resources_stats(context->resources, &before) == XR_COMPILE_RESOURCE_OK);
        size_t attempts = effects_compile_attempts;
        XrXirDiagnostic diagnostic = {0};
        XrXirStatus actual = xr_xir_compile_checked_read(context, input->bytes, input->length,
            &out, &diagnostic);
        printf("role-reject name=%s occupied=%u actual=%u expected=%u diagnostic=%u f=%u b=%u i=%u reason=%u\n",
            input->name, occupied, actual, expected, diagnostic.status, diagnostic.function,
            diagnostic.block, diagnostic.instruction, diagnostic.reason);
        CHECK(actual == expected);
        CHECK(out == before_out);
        CHECK(xr_compile_resources_stats(context->resources, &after) == XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes == before.live_bytes);
        if (early) {
            CHECK(effects_compile_attempts == attempts && before.allocated_bytes == after.allocated_bytes);
            CHECK(before.allocation_count == after.allocation_count && before.peak_bytes == after.peak_bytes);
        } else CHECK(effects_compile_attempts > attempts);
    }
    printf("packet rejection=%s status=%u occupied-and-empty=1 early=%u live-refund=1\n",
        input->name, expected, early);
}

static void role_packet_cases(const XrXirCompileContext *context) {
    const RolePacket accepted[] = {
        {"Task_i64_string", current_task_types67, sizeof(current_task_types67)},
        {"Task_Unit_string", current_task_unit67, sizeof(current_task_unit67)},
        {"direct_GO", current_task_go67, sizeof(current_task_go67)},
        {"five_roles", current_roles67, sizeof(current_roles67)}};
    const RolePacket previous[] = {
        {"complete_Atomic65", old_atomic65, sizeof(old_atomic65)},
        {"complete_TaskTypes66", old_task_types66, sizeof(old_task_types66)},
        {"complete_GO66", old_task_go66, sizeof(old_task_go66)}};
    const RolePacket invalid[] = {
        {"canonical_Unit", bad67_canonical_unit, sizeof(bad67_canonical_unit)},
        {"exported_initializer", bad67_initializer_exported, sizeof(bad67_initializer_exported)},
        {"i64_initializer", bad67_initializer_i64, sizeof(bad67_initializer_i64)},
        {"GO_initializer", bad67_go_initializer, sizeof(bad67_go_initializer)}};
    const XrXirStatus statuses[] = {XR_XIR_BAD_TYPE, XR_XIR_BAD_STRUCTURE,
        XR_XIR_BAD_TYPE, XR_XIR_BAD_TYPE};
    for (unsigned i = 0; i < 4; ++i) role_packet_accept(context, &accepted[i]);
    for (unsigned i = 0; i < 3; ++i) role_packet_reject(context, &previous[i], XR_XIR_BAD_STRUCTURE, true);
    for (unsigned i = 0; i < 4; ++i) role_packet_reject(context, &invalid[i], statuses[i], false);
}

static XrXirArtifact *role_lower(const XrXirCompileContext *context) {
    uint8_t bytes[sizeof(current_roles67)];
    memcpy(bytes, current_roles67, sizeof(bytes));
    XrXirArtifact *checked = NULL, *closed = NULL, *lowered = NULL;
    CHECK(xr_xir_compile_checked_read(context, bytes, sizeof(bytes), &checked, NULL) == XR_XIR_OK);
    memset(bytes, 0xa5, sizeof(bytes));
    CHECK(xr_xir_compile_specialize(checked, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_OK);
    role_shape(xr_xir_compile_artifact_module(closed));
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered, NULL) == XR_XIR_OK);
    role_shape(xr_xir_compile_artifact_module(lowered));
    return lowered;
}

static void role_code_release(void *pointer) {
    RoleCodeOwner *owner = pointer;
    ++role_code_releases;
    xr_xir_compile_artifact_free(owner->lowered);
    xr_compile_resources_free(owner);
}

static XrXirProgram *role_program(const XrXirCompileContext *context, unsigned mode) {
    RoleCodeOwner *owner = NULL;
    void *memory = NULL;
    CHECK(xr_compile_resources_alloc(context->resources, sizeof(*owner), &memory) == XR_COMPILE_RESOURCE_OK);
    owner = memory;
    memset(owner, 0, sizeof(*owner));
    owner->lowered = role_lower(context);
    const XrXirModule *module = xr_xir_compile_artifact_module(owner->lowered);
    XrXirCallEntry entries[5];
    for (uint32_t f = 0; f < 5; ++f) {
        CHECK(xr_xir_compile_vm_bind(owner->lowered, f, &owner->bindings[f], &entries[f]) == XR_XIR_OK);
#ifdef XR_PACKET_ROLES_NATIVE
        bool native = mode == 1 || (mode == 2 && f == 3) || (mode == 3 && f != 3);
        if (native) entries[f] = packet_roles_program.entries[f];
#else
        CHECK(mode == 0);
#endif
    }
    XrXirProgramSpec spec = {
        .abi_version = XR_XIR_PROGRAM_ABI_VERSION,
        .target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION},
        .entries = entries, .entry_count = 5, .declarations = module->declarations,
        .code = {owner, role_code_release}, .types = module->types,
        .proof = xr_xir_compile_program_proof(owner->lowered)};
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(context, &spec, &program) == XR_XIR_OK);
    return program;
}

static void role_trace(void *context, XrXirLifecycleEvent event, uint32_t index) {
    RoleTrace *trace = context;
    CHECK(!index);
    if (event == XR_XIR_MODULE_BEGIN) {
        CHECK(!trace->begin && !trace->ready); ++trace->begin;
    } else {
        CHECK(event == XR_XIR_MODULE_READY && trace->begin == 1 && !trace->ready); ++trace->ready;
    }
}

static XrXirOutputStatus role_output(void *context, const XrXirOutputGroup *group) {
    RoleTrace *trace = context;
    const int64_t expected[] = {5, 41};
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1 && group->values);
    CHECK(trace->output < 2 && group->values[0].type == XR_XIR_I64 && !group->values[0].reserved);
    CHECK(group->values[0].payload == expected[trace->output]); ++trace->output;
    return XR_XIR_OUTPUT_OK;
}

static XrXirValue role_execute(XrXirInstance *instance, uint32_t entry) {
    XrXirInstanceResult result = {0};
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    unsigned steps = 0;
    do {
        result = xr_xir_instance_poll_bounded(instance, 1);
        CHECK(++steps < 4096);
    } while (result.outcome.status == XR_XIR_CALL_READY);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && !result.outcome.wake);
    XrXirValue occupied = {XR_XIR_I64, 0, 99};
    unsigned char before[sizeof(occupied)]; memcpy(before, &occupied, sizeof(before));
    CHECK(xr_xir_instance_take_result(instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(before, &occupied, sizeof(before)));
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    return value;
}

static void role_escape(const XrXirValue *task) {
    XrXirValue alias = {0};
    CHECK(xr_xir_value_copy(task, &alias) == XR_XIR_VALUE_OK);
    CHECK(alias.type == task->type && alias.payload == task->payload && task->payload);
    XrXirCallResult occupied;
    memset(&occupied, 0, sizeof(occupied));
    occupied.status = XR_XIR_CALL_RETURNED; occupied.value = (XrXirValue){XR_XIR_I64, 0, 99};
    unsigned char before[sizeof(occupied)]; memcpy(before, &occupied, sizeof(before));
    CHECK(xr_xir_task_copy_outcome(&alias, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!memcmp(before, &occupied, sizeof(before))); xr_xir_call_result_drop(&occupied);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        XrXirCallResult result = {0};
        CHECK(xr_xir_task_copy_outcome(repeat ? &alias : task, &result) == XR_XIR_CALL_RETURNED);
        CHECK(result.status == XR_XIR_CALL_RETURNED && !result.wake && xr_xir_panic_empty(&result.panic));
        role_zero(&result.value); xr_xir_call_result_drop(&result);
    }
    xr_xir_value_drop(&alias);
}

static void role_execute_pair(const XrXirCompileContext *context, unsigned mode, uint32_t entry) {
    unsigned releases = role_code_releases;
    XrXirProgram *program = role_program(context, mode);
    XrXirInstance *instances[2] = {0};
    XrXirValue values[2] = {{0}};
    RoleTrace traces[2] = {{0}};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, role_output, &traces[i]};
        config.trace = role_trace; config.trace_context = &traces[i];
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) {
        size_t attempts = runtime_attempts;
        CHECK(xr_xir_instance_start(instances[i], 0, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_start(instances[i], 3, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(xr_xir_instance_start_test(instances[i], 2) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(runtime_attempts == attempts && xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_NEW);
        values[i] = role_execute(instances[i], entry);
        CHECK(traces[i].begin == 1 && traces[i].ready == 1 && traces[i].output == (entry == 1 ? 1u : 2u));
        XrXirValue again = role_execute(instances[i], 1);
        CHECK(again.type == XR_XIR_I64 && !again.reserved && again.payload == 7);
        xr_xir_value_drop(&again);
        CHECK(traces[i].begin == 1 && traces[i].ready == 1 && traces[i].output == (entry == 1 ? 1u : 2u));
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    if (entry == 4) CHECK(values[0].payload && values[1].payload && values[0].payload != values[1].payload);
    for (unsigned i = 0; i < 2; ++i) {
        if (entry == 4) role_escape(&values[i]);
        else if (entry == 2) role_zero(&values[i]);
        else CHECK(values[i].type == XR_XIR_I64 && !values[i].reserved && values[i].payload == 7);
        xr_xir_value_drop(&values[i]);
    }
    CHECK(role_code_releases == releases + 1 && !runtime_live && !runtime_bytes);
    printf("roles mode=%u entry=%u instances=2 init-output5-once=1 worker-output41=%u owned-after-free=1 physical=0/0\n",
        mode, entry, entry != 1);
}

static void role_emit(const XrXirCompileContext *context, const char *path) {
    XrXirArtifact *lowered = role_lower(context);
    XrXirCSource source = {0};
    CHECK(xr_xir_compile_emit_c(lowered, "packet_roles", 1048576, &source) == XR_XIR_OK);
    xr_xir_compile_artifact_free(lowered);
    CHECK(source.text && !source.text[source.length] && !strstr(source.text, "({"));
    FILE *file = fopen(path, "wb"); CHECK(file);
    CHECK(fwrite(source.text, 1, source.length, file) == source.length && !fclose(file));
    xr_xir_compile_c_source_free(&source);
}

static void role_invalid_task(void) {
    XrXirValue invalid[] = {{0}, {XR_XIR_UNIT, 0, 1}, {XR_XIR_UNIT, 1, 0}, {XR_XIR_BOOL, 0, 0}};
    for (unsigned i = 0; i < 4; ++i) {
        XrXirCallResult result; memset(&result, 0, sizeof(result));
        unsigned char before[sizeof(result)]; memcpy(before, &result, sizeof(before));
        CHECK(xr_xir_task_copy_outcome(&invalid[i], &result) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!memcmp(before, &result, sizeof(before)));
    }
}

int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 3 && !strcmp(argv[1], "--emit-c")));
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    role_packet_cases(context);
    if (argc == 3) role_emit(context, argv[2]);
    else {
        unsigned modes = 1;
#ifdef XR_PACKET_ROLES_NATIVE
        modes = 4;
#endif
        const uint32_t entries[] = {1, 2, 4};
        for (unsigned mode = 0; mode < modes; ++mode)
            for (unsigned i = 0; i < 3; ++i) role_execute_pair(context, mode, entries[i]);
        role_invalid_task();
    }
    effects_source_owners_free();
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    puts("current67 packet roles and Unit task candidate completed"); return 0;
}
