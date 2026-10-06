/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_unit.c - Unit tasks retain an owned sticky empty success value
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_task.h"
#include "xir/xxir_error.h"
#include "xir/xxir_effects.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_task_go66_golden.h"
#include "xir_task_go67_golden.h"
#include "xir_task_go68_golden.h"
#include "xir_task_unit68_golden.h"

typedef struct UnitOracle { const char *name, *entry; bool escaped, unit_root, error; unsigned outputs; } UnitOracle;
static const UnitOracle unit_oracles[] = {
    {"inferred", "main", false, false, false, 1}, {"explicit_unit", "main", false, false, false, 1},
    {"explicit_null", "main", false, false, false, 1}, {"generic", "main", false, false, false, 1},
    {"escape", "handle", true, false, false, 1}, {"unit_root", "main", false, true, false, 1},
    {"unknown_waiter", "main", false, false, false, 1}, {"escaped_error", "handle", true, false, true, 0}};

static void unit_empty(const XrXirValue *value) { CHECK(!value->type && !value->reserved && !value->payload); }
static XrXirOutputStatus unit_output(void *context, const XrXirOutputGroup *group) {
    unsigned *count = context;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->count == 1 && group->line);
    CHECK(group->values[0].type == XR_XIR_I64 && !group->values[0].reserved && group->values[0].payload == 41);
    CHECK(!*count); ++*count; return XR_XIR_OUTPUT_OK;
}
static void unit_shape(const XrXirModule *module) {
    unsigned tasks = 0;
    CHECK(module->types);
    for (uint32_t t = 0; t < module->types->count; ++t) {
        const XrXirTypeNode *node = &module->types->nodes[t];
        if (node->kind == XR_XIR_TYPE_TASK) { CHECK(node->element == XR_XIR_UNIT && !node->parameter_span); ++tasks; }
    }
    CHECK(tasks == 1);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op == XR_XIR_TASK_AWAIT) {
                const XrXirTypeNode *task = xr_xir_type_node(module->types, xr_xir_operand_type(function, op->args[0]));
                CHECK(task && task->kind == XR_XIR_TYPE_TASK && task->element == XR_XIR_UNIT);
                const XrXirInstruction *normal = &function->instructions[function->blocks[op->targets[0]].first];
                CHECK(normal->op != XR_XIR_INVOKE_RESULT && normal->op != XR_XIR_INVOKE_ERROR);
            }
        }
    }
}
static XrXirProgram *unit_program(const UnitOracle *oracle, uint32_t *entry) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/%s.xr", XR_TASK_UNIT_FIXTURES, oracle->name) > 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_TASK_UNIT_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult source = {0}; XrXirSourceDiagnostic source_diagnostic = {0}; char *failure = NULL;
    XrXirDiagnostic diagnostic = {0}; XrXirCheckedPacket packet = {0};
    XrXirArtifact *read = NULL, *closed = NULL, *lowered = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &source, &source_diagnostic, &failure);
    xr_compile_session_free(session);
    if (status != XR_XIR_OK) fprintf(stderr, "%s check=%u %s\n", oracle->name, status, source_diagnostic.message);
    CHECK(status == XR_XIR_OK && source.checked && !failure);
    unit_shape(xr_xir_compile_artifact_module(source.checked));
    if (!strcmp(oracle->name, "unknown_waiter")) {
        const XrXirModule *checked = xr_xir_compile_artifact_module(source.checked);
        XrXirEffects *effects = NULL; unsigned matches = 0;
        CHECK(xr_xir_compile_effects_analyze(source.checked, &effects) == XR_XIR_OK);
        for (uint32_t f = 0; f < checked->function_count; ++f)
            if (checked->functions[f].name_length == 4 && !memcmp(checked->functions[f].name, "wait", 4)) {
                CHECK(xr_xir_effects_function(effects, f)->throws == XR_XIR_EFFECT_MAY &&
                    xr_xir_effects_function(effects, f)->suspend == XR_XIR_EFFECT_MAY);
                CHECK(xr_xir_effects_error_unidentified(effects, f)); ++matches;
            }
        CHECK(matches == 1); xr_xir_compile_effects_free(effects);
    }
    CHECK(xr_xir_compile_checked_write(source.checked, &packet, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &read, &diagnostic) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(read, &closed, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_artifact_verify(closed, &diagnostic) == XR_XIR_OK);
    unit_shape(xr_xir_compile_artifact_module(closed));
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    status = xr_xir_compile_lower(closed, &target, &lowered, &diagnostic);
    xr_xir_compile_artifact_free(closed);
    if (status != XR_XIR_OK) fprintf(stderr, "%s lower=%u f=%u instruction=%u\n", oracle->name,
        status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK && lowered);
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
static void unit_sticky_value(const XrXirCallResult *outcome, bool error) {
    CHECK(outcome->status == (error ? XR_XIR_CALL_THROWN : XR_XIR_CALL_RETURNED) && !outcome->wake);
    CHECK(xr_xir_panic_empty(&outcome->panic));
    if (!error) { unit_empty(&outcome->value); return; }
    XrXirValue underlying = outcome->value;
    if (underlying.type == XR_XIR_ERROR) CHECK(xr_xir_error_borrow(&outcome->value, &underlying));
    XrXirEnumBorrow value = {0}; CHECK(xr_xir_enum_borrow(&underlying, &value) == XR_XIR_VALUE_OK);
    CHECK(value.name.length == 7 && !memcmp(value.name.bytes, "Problem", 7) && value.member.length == 6 && !memcmp(value.member.bytes, "Failed", 6) && value.field_count == 1);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&value.fields[0], &bytes, &length) && length == 5 && !memcmp(bytes, "owned", 5));
}
static void unit_execute(const UnitOracle *oracle) {
    uint32_t entry = UINT32_MAX; XrXirProgram *program = unit_program(oracle, &entry);
    XrXirInstance *instances[2] = {NULL, NULL}; XrXirValue values[2] = {{0}}; unsigned outputs[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, unit_output, &outputs[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], entry, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = {0}; unsigned steps = 0;
        do { result = xr_xir_instance_poll_bounded(instances[i], 1); CHECK(++steps < 4096); }
        while (result.outcome.status == XR_XIR_CALL_READY);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && outputs[i] == oracle->outputs);
        CHECK(xr_xir_instance_take_result(instances[i], &values[i]) == XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    if (oracle->escaped) CHECK(values[0].payload != values[1].payload);
    for (unsigned i = 0; i < 2; ++i) {
        if (oracle->escaped) {
            XrXirValue copy = {0}; CHECK(xr_xir_value_copy(&values[i], &copy) == XR_XIR_VALUE_OK);
            CHECK(copy.payload == values[i].payload && copy.type == values[i].type);
            XrXirCallResult occupied = {.status = XR_XIR_CALL_RETURNED, .value = {XR_XIR_I64, 0, 99}};
            CHECK(xr_xir_task_copy_outcome(&copy, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
            CHECK(occupied.status == XR_XIR_CALL_RETURNED && occupied.value.type == XR_XIR_I64 && occupied.value.payload == 99);
            xr_xir_call_result_drop(&occupied);
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                XrXirCallResult outcome = {0};
                CHECK(xr_xir_task_copy_outcome(repeat ? &copy : &values[i], &outcome) ==
                    (oracle->error ? XR_XIR_CALL_THROWN : XR_XIR_CALL_RETURNED));
                unit_sticky_value(&outcome, oracle->error); xr_xir_call_result_drop(&outcome);
            }
            xr_xir_value_drop(&copy);
        } else if (oracle->unit_root) unit_empty(&values[i]);
        else CHECK(values[i].type == XR_XIR_I64 && !values[i].reserved && values[i].payload == 7);
        xr_xir_value_drop(&values[i]);
    }
    CHECK(!runtime_live && !runtime_bytes);
    printf("Task<Unit> %s: two Instances, fixed 7/Unit/Error, output41=%u once, escape=%u; physical=0/0\n",
        oracle->name, oracle->outputs, oracle->escaped);
}
static void unit_reject(const char *name) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/rejected/%s.xr", XR_TASK_UNIT_FIXTURES, name) > 0);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_TASK_UNIT_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, context, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, &failure);
    xr_compile_session_free(session);
    if (status == XR_XIR_OK) fprintf(stderr, "unexpected Unit admission: %s\n", name);
    CHECK(status != XR_XIR_OK && !result.checked && !result.snapshot);
    xr_compile_resources_free(failure); xr_xir_compile_source_result_free(&result);
    printf("Task<Unit> reject %s: status=%u, no owned artifact\n", name, status);
}
static void unit_fixed_codec(void) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrXirTypeNode nodes[] = {{.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_UNIT},
        {.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_STRING}};
    XrXirTypes types = {nodes, 2, NULL, NULL}; XrXirType parameters[] = {256, 257};
    XrXirInstruction instruction = {.op = XR_XIR_RETURN}; XrXirBlock block = {.count = 1};
    XrXirFunction function = {.name = "taskTypes", .name_length = 9, .parameters = parameters,
        .parameter_count = 2, .blocks = &block, .block_count = 1, .instructions = &instruction, .instruction_count = 1};
    XrXirModule module = {.stage = XR_XIR_BUILT, .functions = &function, .function_count = 1, .types = &types};
    XrXirArtifact *checked = NULL, *decoded = NULL; XrXirCheckedPacket packet = {0}; XrXirTypes *clone = NULL;
    _Static_assert(XR_XIR_RETURN == 33, "fixed Unit codec RETURN ordinal");
    CHECK(xr_xir_compile_check(context, &module, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == sizeof(task_unit68_golden) && !memcmp(packet.bytes, task_unit68_golden, packet.length));
    CHECK(xr_xir_compile_checked_read(context, task_unit68_golden, sizeof(task_unit68_golden), &decoded, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_types_clone(context, xr_xir_compile_artifact_module(decoded)->types, &clone) == XR_XIR_OK);
    CHECK(clone->nodes != nodes && clone->nodes[0].kind == XR_XIR_TYPE_TASK && clone->nodes[0].element == XR_XIR_UNIT);
    memset(nodes, 0xa5, sizeof(nodes)); memset(parameters, 0xa5, sizeof(parameters));
    CHECK(xr_xir_compile_artifact_verify(checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_artifact_verify(decoded, NULL) == XR_XIR_OK);
    xr_xir_compile_types_free(clone); xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_artifact_free(decoded); xr_xir_compile_artifact_free(checked);
    puts("Task<Unit> independent221B named-role codec and deep clone: current68 writer/read exact");
}
static void unit_old_packet_reject(void) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrXirArtifact *out = NULL; size_t before = effects_compile_attempts;
    CHECK(xr_xir_compile_checked_read(context, task_go66_golden, sizeof(task_go66_golden), &out, NULL) ==
        XR_XIR_BAD_STRUCTURE && !out && effects_compile_attempts == before);
    out = (XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(context, task_go66_golden, sizeof(task_go66_golden), &out, NULL) ==
        XR_XIR_BAD_STRUCTURE && out == (XrXirArtifact *)(uintptr_t)1 && effects_compile_attempts == before);
    out = NULL;
    CHECK(xr_xir_compile_checked_read(context, task_go67_golden, sizeof(task_go67_golden), &out, NULL) ==
        XR_XIR_BAD_STRUCTURE && !out && effects_compile_attempts == before);
    out = (XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(context, task_go67_golden, sizeof(task_go67_golden), &out, NULL) ==
        XR_XIR_BAD_STRUCTURE && out == (XrXirArtifact *)(uintptr_t)1 && effects_compile_attempts == before);
    CHECK(sizeof(task_go68_golden) == sizeof(task_go66_golden) &&
        !memcmp(task_go68_golden + 64, task_go66_golden + 64, sizeof(task_go66_golden) - 64));
    out = NULL;
    CHECK(xr_xir_compile_checked_read(context, task_go68_golden, sizeof(task_go68_golden), &out, NULL) == XR_XIR_OK && out);
    xr_xir_compile_artifact_free(out);
    XrXirValue not_task = {XR_XIR_BOOL, 0, 0}; XrXirCallResult outcome = {0};
    CHECK(xr_xir_task_copy_outcome(&not_task, &outcome) == XR_XIR_CALL_BAD_ARGUMENT && xr_xir_call_result_empty(&outcome));
    not_task = (XrXirValue){0};
    CHECK(xr_xir_task_copy_outcome(&not_task, &outcome) == XR_XIR_CALL_BAD_ARGUMENT && xr_xir_call_result_empty(&outcome));
    puts("Task<Unit> old complete66/67 packets empty/occupied: early reject, zero allocation; independent same-body68 positive; false/Unit not handles");
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    for (unsigned i = 0; i < sizeof(unit_oracles) / sizeof(*unit_oracles); ++i) unit_execute(&unit_oracles[i]);
    const char *rejects[] = {"construct", "invariance", "void", "bare_null", "ordinary_unit_arg", "generic_missing", "non_task", "array_unit"};
    for (unsigned i = 0; i < sizeof(rejects) / sizeof(*rejects); ++i) unit_reject(rejects[i]);
    unit_fixed_codec(); unit_old_packet_reject(); effects_source_owners_free();
    printf("Task<Unit> private sizes: Executor=%zu Task=%zu Call=%zu Frame=%zu state=%u; identity=25/68/22/28/29\n",
        sizeof(XrXirTaskExecutor), sizeof(XirTask), sizeof(XrXirCall), sizeof(CallFrame),
        (unsigned)((sizeof(CallFrame) + 15u) & ~(size_t)15u));
    return 0;
}
