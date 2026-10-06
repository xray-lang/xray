/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_fault_cleanup.c - Host failure executes registered language cleanup
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_task_fault_cleanup_fixture.h"
typedef struct FaultCleanupOutput { int64_t trace[8]; unsigned count; bool rejected; } FaultCleanupOutput;
static XrXirOutputStatus fault_cleanup_output(void *context, const XrXirOutputGroup *group) {
    FaultCleanupOutput *output = context;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1);
    CHECK(group->values[0].type == XR_XIR_I64 && output->count < 8);
    int64_t value = group->values[0].payload;
    CHECK(value == 7 || value == 6 || value == 5);
    output->trace[output->count++] = value;
    if (value == 7 && !output->rejected) { output->rejected = true; return XR_XIR_OUTPUT_ERROR; }
    return XR_XIR_OUTPUT_OK;
}
static XrXirProgram *fault_cleanup_program(unsigned mode) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    TaskFaultFixture fixture; task_fault_fixture(&fixture, mode);
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0}; XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_check(context, &fixture.module, &checked, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "mode=%u check=%u function=%u op=%u\n", mode, status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &packet, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); memset(&fixture, 0xa5, sizeof(fixture));
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, &diagnostic) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    if (mode == TASK_FAULT_CALL) CHECK(!program->arena && !program->types);
    return program;
}
static XrXirInstanceResult fault_cleanup_poll(XrXirInstance *instance) {
    XrXirInstanceResult result = {0}; unsigned transitions = 0;
    do { result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++transitions < 3000); }
    while (result.outcome.status == XR_XIR_CALL_READY);
    return result;
}
static void fault_cleanup_trace(const FaultCleanupOutput *output, unsigned mode, unsigned repeat) {
    unsigned stride = mode == TASK_FAULT_BODY ? 2 : 3;
    CHECK(output->count == stride * repeat);
    for (unsigned i = 0; i < repeat; ++i) {
        CHECK(output->trace[i * stride] == 7 && output->trace[i * stride + 1] == 6);
        if (stride == 3) CHECK(output->trace[i * stride + 2] == 5);
    }
}
static void fault_cleanup_execute(unsigned mode) {
    XrXirProgram *program = fault_cleanup_program(mode);
    XrXirInstance *instances[2] = {0}; FaultCleanupOutput outputs[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, fault_cleanup_output, &outputs[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    CHECK(instances[0]->domain != instances[1]->domain);
    xr_xir_compile_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstance *instance = instances[i];
        CHECK(xr_xir_instance_start(instance, 0, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = fault_cleanup_poll(instance);
        fprintf(stderr, "fault cleanup mode=%u instance=%u status=%u calls=%u first=%lld incomplete=%u\n", mode, i,
            result.outcome.status, outputs[i].count, (long long)outputs[i].trace[0],
            instance->executor->cleanup_incomplete || xr_xir_call_cleanup_incomplete(instance->call));
        CHECK(result.outcome.status == XR_XIR_CALL_OUTPUT_ERROR && instance->state == XR_XIR_INSTANCE_READY);
        fault_cleanup_trace(&outputs[i], mode, 1);
        CHECK(!instance->executor->cleanup_incomplete && !xr_xir_call_cleanup_incomplete(instance->call));
        XrXirValue sentinel = {XR_XIR_I64, 0, 99};
        CHECK(xr_xir_instance_take_result(instance, &sentinel) ==
            (mode == TASK_FAULT_CALL ? XR_XIR_CALL_BAD_ARGUMENT : XR_XIR_CALL_OUTPUT_ERROR));
        CHECK(sentinel.type == XR_XIR_I64 && sentinel.payload == 99);
        XrXirValue empty = {0};
        CHECK(xr_xir_instance_take_result(instance, &empty) ==
            (mode == TASK_FAULT_CALL ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_OUTPUT_ERROR));
        CHECK(!empty.type && !empty.reserved && !empty.payload);
        uint64_t epoch = instance->epoch;
        XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instance->domain);
        CHECK(xr_xir_instance_start(instance, UINT32_MAX, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT && instance->epoch == epoch);
        CHECK(fault_cleanup_poll(instance).outcome.status == XR_XIR_CALL_OUTPUT_ERROR);
        CHECK(xr_xir_instance_start(instance, 0, NULL, 0) == XR_XIR_CALL_READY && instance->epoch == epoch + 1);
        result = fault_cleanup_poll(instance);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
        fault_cleanup_trace(&outputs[i], mode, 2);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED && value.type == XR_XIR_I64 && value.payload == 7);
        XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instance->domain);
        CHECK(after.bound && after.requested_bytes >= before.requested_bytes &&
            after.requested_call_bytes > before.requested_call_bytes && after.work > before.work);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        CHECK(value.type == XR_XIR_I64 && value.payload == 7); xr_xir_value_drop(&value);
    }
    CHECK(!runtime_live && !runtime_bytes);
    printf("fault cleanup mode=%u: two independent Instances, actual7/6%s, original13 then new7, same cumulative budget, physical=0/0\n",
        mode, mode == TASK_FAULT_BODY ? "" : "/5");
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    unsigned mode = !strcmp(argv[1], "body") ? TASK_FAULT_BODY : !strcmp(argv[1], "call") ? TASK_FAULT_CALL :
        !strcmp(argv[1], "await") ? TASK_FAULT_AWAIT : TASK_FAULT_CASES;
    CHECK(mode < TASK_FAULT_CASES);
    setvbuf(stdout, NULL, _IONBF, 0);
    fault_cleanup_execute(mode); effects_source_owners_free(); return 0;
}
