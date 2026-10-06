/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_root_fault.c - Completed roots preserve child shutdown faults
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
    CHECK(xr_xir_compile_check(context, &fixture.module, &checked, &diagnostic) == XR_XIR_OK);
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
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    for (unsigned mode = 0; mode < 5; ++mode) root_fault_case(mode);
    effects_source_owners_free();
    puts("Completed root host faults stay visible through poll/take/stop/free; ordinary child panic remains isolated; physical=0/0");
    return 0;
}
