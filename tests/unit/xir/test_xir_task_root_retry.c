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
#include "xir/xxir_emit_c.h"
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
static XrXirProgram *root_fault_program(bool panic, bool initializer_fault) {
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
    XrXirInstruction init[] = {{.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7},
        {.op = XR_XIR_PRINT, .args = {0, 1}}, {.op = XR_XIR_RETURN}};
    if (initializer_fault) {
        fixture.functions[2].instructions = init; fixture.functions[2].instruction_count = 3;
        fixture.init_block.count = 3; fixture.functions[2].operands = &fixture.argument;
        fixture.functions[2].operand_count = 1;
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

/* The same GO/return7/child-output graph takes one of two executable paths.
 * Both have an EXIT entry; only the registration path owes a language cleanup. */
static XrXirProgram *root_cleanup_program(bool registered, const char *emit_path) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    TaskVmFixture f; task_vm_fixture(&f, TASK_VM_I64);
    f.root[2] = (XrXirInstruction){.op = XR_XIR_RETURN};
    f.root_blocks[0].count = f.functions[0].instruction_count = 3; f.functions[0].block_count = 1;
    XrXirInstruction worker[] = {
        {.op = XR_XIR_SUSPEND},
        {.op = XR_XIR_CONST_BOOL, .type = XR_XIR_BOOL, .immediate = registered},
        {.op = XR_XIR_BRANCH, .args = {2}, .targets = {1, 2}},
        {.op = XR_XIR_CLEANUP_REGISTER, .targets = {3}, .immediate = 3},
        {.op = XR_XIR_PRINT, .args = {0, 1}}, {.op = XR_XIR_RETURN},
        {.op = XR_XIR_PRINT, .args = {1, 1}}, {.op = XR_XIR_RETURN}};
    XrXirBlock blocks[] = {{.count = 3}, {.first = 3, .count = 1},
        {.first = 4, .count = 2}, {.first = 6, .count = 2, .frontier = 4}};
    XrXirInstruction cleanup[] = {{.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7},
        {.op = XR_XIR_PRINT, .args = {0, 1}}, {.op = XR_XIR_RETURN}};
    XrXirBlock cleanup_block = {.count = 3}; uint32_t operand = 0, worker_operands[] = {0, 0};
    XrXirFunction functions[4]; memcpy(functions, f.functions, sizeof(f.functions));
    functions[1].instructions = worker; functions[1].instruction_count = 8;
    functions[1].blocks = blocks; functions[1].block_count = 4;
    functions[1].operands = worker_operands; functions[1].operand_count = 2;
    functions[3] = (XrXirFunction){.name = "cleanup", .name_length = 7, .instructions = cleanup,
        .instruction_count = 3, .blocks = &cleanup_block, .block_count = 1, .operands = &operand, .operand_count = 1};
    XrXirFunctionIdentity identities[4] = {0}; identities[0].exported = 1; identities[3].cleanup_owner = 2;
    f.declarations.functions = identities; f.module.functions = functions; f.module.function_count = 4;
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0}; XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xir_fixture_check(context, &f.module, &checked, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "cleanup graph check=%u function=%u op=%u\n", status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &packet, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    memset(&f, 0xa5, sizeof(f)); memset(worker, 0xa5, sizeof(worker)); memset(cleanup, 0xa5, sizeof(cleanup));
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, &diagnostic) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    if (emit_path) {
        XrXirCSource code = {0};
        CHECK(xr_xir_compile_emit_c(lowered, "legacy_cleanup", UINT64_C(4194304), &code) == XR_XIR_OK);
        FILE *file = fopen(emit_path, "wb"); CHECK(file);
        CHECK(fwrite(code.text, 1, code.length, file) == code.length && fclose(file) == 0);
        xr_xir_compile_c_source_free(&code);
    }
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    return program;
}
static XrXirInstanceResult root_retry_poll(XrXirInstance *instance) {
    XrXirInstanceResult result = {0}; unsigned steps = 0;
    do { result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++steps < 1000); }
    while (result.outcome.status == XR_XIR_CALL_READY);
    return result;
}
static void root_retry_case(unsigned mode) {
    XrXirProgram *program = mode < 2 || mode == 4 ? root_fault_program(false, false) : root_cleanup_program(mode == 2, NULL);
    RootFaultOutput output = {XR_XIR_OUTPUT_ERROR, 0}; XrXirInstanceConfig config;
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, root_fault_output, &output};
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, 0, NULL, 0) == XR_XIR_CALL_READY);
    unsigned fault_outputs = mode == 2 ? 2 : 1;
    CHECK(root_retry_poll(instance).outcome.status == XR_XIR_CALL_OUTPUT_ERROR && output.calls == fault_outputs);
    CHECK(instance->state == XR_XIR_INSTANCE_READY);
    uint64_t epoch = instance->epoch; XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instance->domain);
    CHECK(xr_xir_instance_start(instance, UINT32_MAX, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT && instance->epoch == epoch);
    CHECK(root_retry_poll(instance).outcome.status == XR_XIR_CALL_OUTPUT_ERROR);
    XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_OUTPUT_ERROR);
    CHECK(!value.type && !value.payload && !value.reserved);
    if (mode == 1) CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_OUTPUT_ERROR);
    if (mode == 4) {
        XrXirDomainBudgetStats stats = xr_xir_domain_budget_stats(instance->domain);
        CHECK(stats.work < stats.work_limit);
        CHECK(xr_xir_domain_work(instance->domain, stats.work_limit - stats.work));
        CHECK(xr_xir_domain_budget_stats(instance->domain).work == stats.work_limit);
    }
    output.status = XR_XIR_OUTPUT_OK;
    XrXirCallStatus started = xr_xir_instance_start(instance, 0, NULL, 0);
    bool blocked = mode == 1 || mode == 2 || mode == 4;
    if (blocked) {
        CHECK(started == (mode == 4 ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_BAD_STATE) && instance->epoch == epoch);
        CHECK(root_retry_poll(instance).outcome.status == XR_XIR_CALL_OUTPUT_ERROR && output.calls == fault_outputs);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_OUTPUT_ERROR);
    } else {
        CHECK(started == XR_XIR_CALL_READY && instance->epoch == epoch + 1);
        XrXirInstanceResult result = root_retry_poll(instance);
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && output.calls == 2);
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED && value.type == XR_XIR_I64 && value.payload == 7);
        XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instance->domain);
        CHECK(after.requested_bytes > before.requested_bytes && after.requested_call_bytes > before.requested_call_bytes && after.work > before.work);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY); xr_xir_value_drop(&value);
    }
    xr_xir_compile_program_drop(program); CHECK(!runtime_live && !runtime_bytes);
    printf("ordinary retry mode=%u registered=%u blocked=%u: retained failed candidate epoch/fault, owned root7 or sticky incomplete cleanup, physical=0/0\n", mode, mode == 2, blocked);
}
static void root_initialization_sticky(void) {
    XrXirProgram *program = root_fault_program(false, true);
    RootFaultOutput output = {XR_XIR_OUTPUT_ERROR, 0}; XrXirInstanceConfig config;
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, root_fault_output, &output};
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, 0, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(root_retry_poll(instance).outcome.status == XR_XIR_CALL_OUTPUT_ERROR && output.calls == 1);
    CHECK(instance->state == XR_XIR_INSTANCE_FAILED);
    uint64_t epoch = instance->epoch; XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instance->domain);
    output.status = XR_XIR_OUTPUT_OK;
    CHECK(xr_xir_instance_start(instance, 0, NULL, 0) == XR_XIR_CALL_OUTPUT_ERROR && instance->epoch == epoch);
    CHECK(root_retry_poll(instance).outcome.status == XR_XIR_CALL_OUTPUT_ERROR && output.calls == 1);
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instance->domain);
    CHECK(before.requested_bytes == after.requested_bytes && before.requested_call_bytes == after.requested_call_bytes &&
        before.work == after.work && before.metadata_live == after.metadata_live && before.call_live == after.call_live);
    CHECK(xr_xir_task_executor_completion_status(instance->executor) == XR_XIR_CALL_OUTPUT_ERROR);
    XrXirCallStatus freed = xr_xir_instance_free(instance);
    fprintf(stderr, "initialization accepted OUTPUT_ERROR free=%u expected=%u\n",
        (unsigned)freed, (unsigned)XR_XIR_CALL_OUTPUT_ERROR);
    CHECK(freed == XR_XIR_CALL_OUTPUT_ERROR);
    xr_xir_compile_program_drop(program); CHECK(!runtime_live && !runtime_bytes);
    puts("initialization FAILED stays sticky, provider recovery creates no activation and resets no budget; physical=0/0");
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 3 && !strcmp(argv[1], "--emit-legacy-cleanup")));
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3) {
        XrXirProgram *program = root_cleanup_program(true, argv[2]);
        xr_xir_compile_program_drop(program); CHECK(!runtime_live && !runtime_bytes);
        effects_source_owners_free(); return 0;
    }
    for (unsigned mode = 0; mode < 5; ++mode) root_retry_case(mode);
    root_initialization_sticky();
    effects_source_owners_free(); return 0;
}
