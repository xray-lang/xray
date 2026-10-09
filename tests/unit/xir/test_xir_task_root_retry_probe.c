/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_root_fault.c - Completed roots preserve child shutdown faults
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_task_vm_fixture.h"

typedef struct RootFaultOutput { XrXirOutputStatus status; unsigned calls; } RootFaultOutput;
static XrXirOutputStatus root_fault_output(void *context, const XrXirOutputGroup *group) {
    RootFaultOutput *output = context;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->count == 1 && group->line);
    CHECK(group->values[0].type == XR_XIR_I64 && group->values[0].payload == 7);
    ++output->calls; return output->status;
}
static XrXirProgram *root_fault_program(bool panic) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    TaskVmFixture fixture; task_vm_fixture(&fixture, panic ? TASK_VM_PANIC : TASK_VM_I64);
    fixture.root[2] = (XrXirInstruction){.op = XR_XIR_RETURN};
    fixture.root_blocks[0].count = fixture.functions[0].instruction_count = 3;
    fixture.functions[0].block_count = 1;
    if (!panic) {
        fixture.worker[1] = (XrXirInstruction){.op = XR_XIR_PRINT, .args = {0, 1}};
        fixture.worker[2] = (XrXirInstruction){.op = XR_XIR_RETURN};
        fixture.worker_block.count = fixture.functions[1].instruction_count = 3;
        fixture.functions[1].operands = &fixture.argument; fixture.functions[1].operand_count = 1;
    }
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0}; XrXirDiagnostic diagnostic = {0};
    CHECK(xir_fixture_check(context, &fixture.module, &checked, &diagnostic) == XR_XIR_OK);
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
    return program;
}
static void root_fault_case(unsigned mode) {
    static const XrXirOutputStatus outputs[] = {XR_XIR_OUTPUT_OK, XR_XIR_OUTPUT_ERROR, XR_XIR_OUTPUT_OOM, XR_XIR_OUTPUT_LIMIT};
    static const XrXirCallStatus faults[] = {XR_XIR_CALL_RETURNED, XR_XIR_CALL_OUTPUT_ERROR, XR_XIR_CALL_OOM, XR_XIR_CALL_LIMIT};
    XrXirProgram *program = root_fault_program(mode == 4);
    XrXirCallStatus expected = mode == 4 ? XR_XIR_CALL_RETURNED : faults[mode];
    for (unsigned pass = 0; pass < 2; ++pass) {
        RootFaultOutput output = {mode == 4 ? XR_XIR_OUTPUT_OK : outputs[mode], 0};
        XrXirInstanceConfig config; XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, root_fault_output, &output};
        CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance, 0, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = {0}; unsigned steps = 0;
        do { result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++steps < 1000); }
        while (result.outcome.status == XR_XIR_CALL_READY);
        CHECK(result.outcome.status == expected && output.calls == (mode == 4 ? 0u : 1u));
        XrXirValue value = {0};
        XrXirCallStatus taken = xr_xir_instance_take_result(instance, &value);
        XrXirCallStatus stopped = xr_xir_instance_stop(instance);
        printf("root mode=%u pass=%u poll=%u take=%u stop=%u payload=%llu\n", mode, pass,
            result.outcome.status, taken, stopped, (unsigned long long)value.payload);
        CHECK(taken == expected);
        CHECK(expected == XR_XIR_CALL_RETURNED ? value.type == XR_XIR_I64 && value.payload == 7 :
            !value.type && !value.payload && !value.reserved);
        CHECK(stopped == (expected == XR_XIR_CALL_RETURNED ? XR_XIR_CALL_READY : expected));
        CHECK(xr_xir_instance_free(instance) == (expected == XR_XIR_CALL_RETURNED ? XR_XIR_CALL_READY : expected));
        if (pass == 1) xr_xir_compile_program_drop(program);
        xr_xir_value_drop(&value);
    }
    CHECK(!runtime_live && !runtime_bytes);
}

static bool root_retry_probe(void) {
    XrXirProgram *program = root_fault_program(false);
    RootFaultOutput output = {XR_XIR_OUTPUT_ERROR, 0};
    XrXirInstanceConfig config; XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, root_fault_output, &output};
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, 0, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult first = {0}; unsigned steps = 0;
    do { first = xr_xir_instance_poll_bounded(instance, 1); CHECK(++steps < 1000); }
    while (first.outcome.status == XR_XIR_CALL_READY);
    CHECK(first.outcome.status == XR_XIR_CALL_OUTPUT_ERROR && output.calls == 1);
    CHECK(instance->state == XR_XIR_INSTANCE_READY);
    uint64_t epoch = instance->epoch;
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instance->domain);
    XrXirCallStatus rejected = xr_xir_instance_start(instance, UINT32_MAX, NULL, 0);
    CHECK(rejected == XR_XIR_CALL_BAD_ARGUMENT && instance->epoch == epoch);
    CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == XR_XIR_CALL_OUTPUT_ERROR);
    output.status = XR_XIR_OUTPUT_OK;
    XrXirCallStatus started = xr_xir_instance_start(instance, 0, NULL, 0);
    CHECK(started == XR_XIR_CALL_READY && instance->epoch == epoch + 1);
    XrXirInstanceResult second = {0}; steps = 0;
    do { second = xr_xir_instance_poll_bounded(instance, 1); CHECK(++steps < 1000); }
    while (second.outcome.status == XR_XIR_CALL_READY);
    CHECK(output.calls == 2);
    XrXirCallResult raw = xr_xir_call_poll_bounded(instance->call, 1);
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instance->domain);
    CHECK(raw.status == XR_XIR_CALL_RETURNED && raw.value.payload == 7);
    CHECK(after.requested_bytes > before.requested_bytes && after.requested_call_bytes > before.requested_call_bytes && after.work > before.work);
    printf("RETRY observation: initialized READY Instance, first child OutputError=%u; invalid start=%u retains epoch=%llu/fault; valid replacement start=%u epoch=%llu; provider recovered and actual calls=%u; new root Call status=%u/value=%llu but public poll=%u; cumulative requested value/call/work=%llu/%llu/%llu -> %llu/%llu/%llu\n",
        first.outcome.status, rejected, (unsigned long long)epoch, started, (unsigned long long)instance->epoch, output.calls,
        raw.status, (unsigned long long)raw.value.payload, second.outcome.status,
        (unsigned long long)before.requested_bytes, (unsigned long long)before.requested_call_bytes, (unsigned long long)before.work,
        (unsigned long long)after.requested_bytes, (unsigned long long)after.requested_call_bytes, (unsigned long long)after.work);
    XrXirCallStatus freed = xr_xir_instance_free(instance);
    printf("RETRY release observation: free=%u\n", freed);
    xr_xir_compile_program_drop(program); CHECK(!runtime_live && !runtime_bytes);
    return second.outcome.status == XR_XIR_CALL_RETURNED && freed == XR_XIR_CALL_READY;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    for (unsigned mode = 0; mode < 5; ++mode) root_fault_case(mode);
    bool recovered = root_retry_probe(); effects_source_owners_free();
    CHECK(recovered); return 0;
}
