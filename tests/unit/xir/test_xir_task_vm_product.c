/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_vm_product.c - Closed Program Task execution and owned escape
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
enum { TASK_PRODUCT_BOOL_RESULT = TASK_VM_MODE_COUNT, TASK_PRODUCT_BOOL_ARGUMENT, TASK_PRODUCT_UNUSED_BOOL, TASK_PRODUCT_NO_ARENA };

static XrXirProgram *task_product_program(uint32_t mode) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    TaskVmFixture fixture; task_vm_fixture(&fixture, mode);
    if (mode == TASK_PRODUCT_BOOL_RESULT) {
        fixture.element = XR_XIR_BOOL; fixture.node.element = XR_XIR_BOOL;
        fixture.root[0] = (XrXirInstruction){.op = XR_XIR_CONST_BOOL, .type = XR_XIR_BOOL, .immediate = 1};
        fixture.root[6].type = XR_XIR_BOOL; fixture.functions[0].result = XR_XIR_BOOL;
        fixture.functions[1].result = XR_XIR_BOOL;
    } else if (mode == TASK_PRODUCT_BOOL_ARGUMENT) {
        fixture.element = XR_XIR_BOOL;
        fixture.root[0] = (XrXirInstruction){.op = XR_XIR_CONST_BOOL, .type = XR_XIR_BOOL, .immediate = 1};
        fixture.worker[1] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7};
        fixture.worker[2] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {2}};
        fixture.worker_block.count = fixture.functions[1].instruction_count = 3;
    } else if (mode == TASK_PRODUCT_UNUSED_BOOL || mode == TASK_PRODUCT_NO_ARENA) {
        fixture.node.element = XR_XIR_BOOL;
        fixture.root[1] = (XrXirInstruction){.op = XR_XIR_RETURN};
        fixture.root_blocks[0].count = fixture.functions[0].instruction_count = 2;
        fixture.functions[0].block_count = 1; fixture.functions[0].operand_count = 0;
        fixture.functions[0].operands = NULL;
        if (mode == TASK_PRODUCT_NO_ARENA) fixture.module.types = NULL;
    }
    XrXirInstruction entry_ops[2] = {
        {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64}, {.op = XR_XIR_RETURN}};
    XrXirBlock entry_block = {.count = 2};
    XrXirFunction functions[4]; XrXirFunctionIdentity identities[4] = {0};
    memcpy(functions, fixture.functions, sizeof(fixture.functions));
    memcpy(identities, fixture.identities, sizeof(fixture.identities));
    functions[3] = (XrXirFunction){.name = "entry", .name_length = 5, .result = XR_XIR_I64,
        .blocks = &entry_block, .block_count = 1, .instructions = entry_ops, .instruction_count = 2};
    fixture.module.functions = functions; fixture.module.function_count = 4;
    fixture.declarations.functions = identities; fixture.declarations.entry_function = 3;
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0}; XrXirDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_check(context, &fixture.module, &checked, &diagnostic) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &packet, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    memset(&fixture, 0xa5, sizeof(fixture));
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, &diagnostic) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, &diagnostic) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    XrXirStatus status = xr_xir_compile_lower(closed, &target, &lowered, &diagnostic);
    xr_xir_compile_artifact_free(closed);
    if (status != XR_XIR_OK) fprintf(stderr, "mode=%u lower status=%u f=%u op=%u\n",
        mode, status, diagnostic.function, diagnostic.instruction);
    CHECK(status == XR_XIR_OK);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    if (mode == TASK_PRODUCT_NO_ARENA) CHECK(!program->arena && !program->types);
    return program;
}
static void task_product_execute(uint32_t mode) {
    XrXirProgram *program = task_product_program(mode);
    for (unsigned pass = 0; pass < 2; ++pass) {
        XrXirInstanceConfig config; XrXirInstance *instance = NULL; XrXirValue value = {0};
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
        if (mode == TASK_PRODUCT_NO_ARENA) {
            XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instance->domain);
            XrXirValue invalid = {0}; XrXirCallRequest request = {0};
            size_t attempts = runtime_attempts;
            CHECK(xr_xir_task_executor_spawn(instance->executor, (XrXirType)256,
                &request, &invalid) == XR_XIR_CALL_BAD_ARGUMENT);
            XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instance->domain);
            CHECK(!invalid.type && !invalid.reserved && !invalid.payload && runtime_attempts == attempts);
            CHECK(after.bound && before.requested_bytes == after.requested_bytes &&
                before.requested_call_bytes == after.requested_call_bytes && before.work == after.work);
        }
        CHECK(xr_xir_instance_start(instance, 0, NULL, 0) == XR_XIR_CALL_READY);
        XrXirInstanceResult result = {0};
        unsigned transitions = 0;
        do { result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++transitions < 1000); }
        while (result.outcome.status == XR_XIR_CALL_READY);
        CHECK(result.outcome.status == (mode == TASK_VM_PANIC ? XR_XIR_CALL_DIVIDE_BY_ZERO : XR_XIR_CALL_RETURNED));
        if (mode != TASK_VM_PANIC) CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        if (mode == TASK_PRODUCT_NO_ARENA) {
            XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instance->domain);
            XrXirValue again = {0};
            CHECK(xr_xir_instance_start(instance, 0, NULL, 0) == XR_XIR_CALL_READY);
            do { result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++transitions < 1000); }
            while (result.outcome.status == XR_XIR_CALL_READY);
            CHECK(result.outcome.status == XR_XIR_CALL_RETURNED);
            CHECK(xr_xir_instance_take_result(instance, &again) == XR_XIR_CALL_RETURNED);
            CHECK(again.type == XR_XIR_I64 && again.payload == 7); xr_xir_value_drop(&again);
            XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instance->domain);
            CHECK(after.bound && after.requested_bytes >= before.requested_bytes &&
                after.requested_call_bytes > before.requested_call_bytes && after.work > before.work);
        }
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        if (pass == 1) xr_xir_compile_program_drop(program);
        if (mode == TASK_VM_I64 || mode == TASK_PRODUCT_BOOL_ARGUMENT ||
            mode == TASK_PRODUCT_UNUSED_BOOL || mode == TASK_PRODUCT_NO_ARENA)
            CHECK(value.type == XR_XIR_I64 && value.payload == 7);
        if (mode == TASK_PRODUCT_BOOL_RESULT)
            CHECK(value.type == XR_XIR_BOOL && !value.reserved && value.payload == 1);
        if (mode == TASK_VM_STRING) {
            const char *bytes = NULL; size_t length = 0;
            CHECK(xr_xir_string_view(&value, &bytes, &length) && length == 3 && !memcmp(bytes, "a\0b", 3));
        }
        if (mode == TASK_VM_DISCARD || mode == TASK_VM_PANIC) CHECK(!value.type && !value.payload && !value.reserved);
        xr_xir_value_drop(&value);
    }
    CHECK(!runtime_live && !runtime_bytes);
    printf("Task VM mode=%u: two real Instances, fixed oracle, producer-first drop, runtime physical=0/0\n", mode);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    task_product_execute(TASK_VM_I64); task_product_execute(TASK_VM_STRING);
    task_product_execute(TASK_VM_DISCARD); task_product_execute(TASK_VM_PANIC);
    task_product_execute(TASK_PRODUCT_UNUSED_BOOL);
    task_product_execute(TASK_PRODUCT_NO_ARENA);
    task_product_execute(TASK_PRODUCT_BOOL_RESULT);
    task_product_execute(TASK_PRODUCT_BOOL_ARGUMENT);
    effects_source_owners_free();
    return 0;
}
