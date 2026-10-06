/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_fault_order.c - Accepted root failure precedes child termination
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_library_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"

enum { ORDER_NORMAL, ORDER_STOP, ORDER_FREE, ORDER_RETRY, ORDER_MODE_COUNT, ORDER_DELAY = 32 };
typedef struct OrderOutput {
    int64_t values[3];
    uint32_t steps[3], count, step;
} OrderOutput;

static XrXirOutputStatus order_output(void *context, const XrXirOutputGroup *group) {
    OrderOutput *output = context;
    CHECK(group->stream == XR_XIR_STDOUT && !group->line && group->count == 1);
    CHECK(group->values[0].type == XR_XIR_I64 && output->count < 3);
    int64_t value = (int64_t)group->values[0].payload;
    static const int64_t expected[] = {7, 9, 6};
    CHECK(value == expected[output->count]);
    output->values[output->count] = value;
    output->steps[output->count++] = output->step;
    return value == 7 ? XR_XIR_OUTPUT_ERROR : value == 9 ? XR_XIR_OUTPUT_OOM : XR_XIR_OUTPUT_OK;
}

static XrXirProgram *order_program(const XrXirCompileContext *context) {
    XrXirInstruction init = {.op = XR_XIR_RETURN};
    XrXirBlock init_block = {.count = 1};
    XrXirInstruction root[] = {
        {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7},
        {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 6},
        {.op = XR_XIR_CLEANUP_REGISTER, .args = {0, 1}, .targets = {1}, .immediate = 2},
        {.op = XR_XIR_GO, .type = 256, .immediate = 3},
        {.op = XR_XIR_OUTPUT, .args = {0}, .immediate = 1},
        {.op = XR_XIR_RETURN, .args = {0}}};
    XrXirBlock root_blocks[] = {{0, 3, 0, 0}, {3, 3, 0, 3}};
    uint32_t capture = 1;
    XrXirType parameter = XR_XIR_I64;
    XrXirInstruction cleanup[ORDER_DELAY + 2] = {0};
    for (uint32_t i = 0; i < ORDER_DELAY; ++i)
        cleanup[i] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = i};
    cleanup[ORDER_DELAY] = (XrXirInstruction){.op = XR_XIR_OUTPUT, .args = {0}, .immediate = 1};
    cleanup[ORDER_DELAY + 1] = (XrXirInstruction){.op = XR_XIR_RETURN};
    XrXirBlock cleanup_block = {.count = ORDER_DELAY + 2};
    XrXirInstruction worker[] = {
        {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 9},
        {.op = XR_XIR_OUTPUT, .args = {0}, .immediate = 1},
        {.op = XR_XIR_RETURN, .args = {0}}};
    XrXirBlock worker_block = {.count = 3};
    XrXirInstruction success[] = {
        {.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 7},
        {.op = XR_XIR_RETURN, .args = {0}}};
    XrXirBlock success_block = {.count = 2};
    XrXirFunction functions[] = {
        {.name = "init", .name_length = 4, .blocks = &init_block, .block_count = 1,
            .instructions = &init, .instruction_count = 1},
        {.name = "root", .name_length = 4, .result = XR_XIR_I64, .blocks = root_blocks, .block_count = 2,
            .instructions = root, .instruction_count = 6, .operands = &capture, .operand_count = 1},
        {.name = "cleanup", .name_length = 7, .parameters = &parameter, .parameter_count = 1,
            .blocks = &cleanup_block, .block_count = 1, .instructions = cleanup, .instruction_count = ORDER_DELAY + 2},
        {.name = "worker", .name_length = 6, .result = XR_XIR_I64, .blocks = &worker_block, .block_count = 1,
            .instructions = worker, .instruction_count = 3},
        {.name = "success", .name_length = 7, .result = XR_XIR_I64, .blocks = &success_block, .block_count = 1,
            .instructions = success, .instruction_count = 2}};
    XrXirFunctionIdentity identities[5] = {{0}};
    identities[1].exported = 1; identities[2].cleanup_owner = 2; identities[4].exported = 1;
    XrXirTypeNode node = {.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_I64};
    XrXirTypes types = {.nodes = &node, .count = 1};
    XrXirSourceModule source = {.name = "fault-order", .name_length = 11};
    XrXirDeclarations declarations = {.modules = &source, .module_count = 1, .functions = identities,
        .root_module = 0, .entry_function = 1};
    XrXirModule built = {.stage = XR_XIR_BUILT, .functions = functions, .function_count = 5,
        .declarations = &declarations, .types = &types, .linkage_kind = XR_XIR_PROGRAM};
    XrXirArtifact *checked = NULL, *read = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0}; XrXirProgram *program = NULL; XrXirDiagnostic diagnostic = {0};
    XrXirStatus status = xr_xir_compile_check(context, &built, &checked, &diagnostic);
    if (status != XR_XIR_OK) fprintf(stderr, "check=%u function=%u block=%u instruction=%u reason=%u\n",
        status, diagnostic.function, diagnostic.block, diagnostic.instruction, diagnostic.reason);
    CHECK(status == XR_XIR_OK);
    memset(root, 0xa5, sizeof(root)); memset(cleanup, 0xa5, sizeof(cleanup));
    memset(worker, 0xa5, sizeof(worker)); memset(functions, 0xa5, sizeof(functions));
    memset(identities, 0xa5, sizeof(identities)); memset(&node, 0xa5, sizeof(node));
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &read, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(read, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_OK);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    CHECK(program && program->arena && program->types);
    return program;
}

static XrXirInstanceResult order_poll(XrXirInstance *instance, OrderOutput *output) {
    CHECK(output->step < 4096);
    XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, 1);
    ++output->step;
    return result;
}

static void order_execute(uint32_t mode) {
    CHECK(!runtime_live && !runtime_bytes);
    LibraryCompileOwner owner = {0};
    CHECK(library_compile_owner_new(&owner, &library_compile_limits) == XR_XIR_OK);
    XrXirProgram *program = order_program(&owner.context);
    XrXirInstance *instances[2] = {NULL, NULL}; OrderOutput outputs[2] = {{0}};
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, order_output, &outputs[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstance *instance = instances[i]; OrderOutput *output = &outputs[i];
        XrXirDomain *domain = instance->domain; CHECK(xr_xir_domain_retain(domain));
        CHECK(xr_xir_instance_start(instance, 1, NULL, 0) == XR_XIR_CALL_READY);
        uint32_t accepted = UINT32_MAX, child_terminal = UINT32_MAX;
        XrXirInstanceResult result = {0};
        while (child_terminal == UINT32_MAX) {
            result = order_poll(instance, output);
            CHECK(result.outcome.status == XR_XIR_CALL_READY);
            if (output->count == 1 && accepted == UINT32_MAX) {
                accepted = output->step - 1;
                CHECK(instance->executor->first_failure == XR_XIR_CALL_OUTPUT_ERROR);
                CHECK(instance->executor->active && instance->executor->shutdown_status == XR_XIR_CALL_READY);
                CHECK(instance->executor->active->object.arena == instance->program->arena);
                const XrXirExecutorBinding binding = {instance->executor, &instance->executor->root,
                    instance->executor->generation, instance->executor->root.ticket};
                XrXirCallStatus failure = XR_XIR_CALL_READY;
                CHECK(xr_xir_call_driver_failure(instance->call, &binding, &failure));
                CHECK(failure == XR_XIR_CALL_OUTPUT_ERROR);
            }
            if (output->count == 2 && !instance->executor->active) {
                child_terminal = output->step - 1;
                CHECK(accepted < child_terminal && output->steps[0] < output->steps[1]);
                CHECK(xr_xir_call_state(instance->call) == XR_XIR_CALL_READY);
                CHECK(instance->executor->shutdown_status == XR_XIR_CALL_OOM);
                CHECK(instance->executor->first_failure == XR_XIR_CALL_OUTPUT_ERROR);
                CHECK(!xr_xir_task_executor_root_idle(instance->executor));
            }
        }
        if (mode == ORDER_STOP) {
            CHECK(xr_xir_instance_stop(instance) == XR_XIR_CALL_OUTPUT_ERROR);
            result = order_poll(instance, output);
        } else if (mode != ORDER_FREE) {
            do { result = order_poll(instance, output); } while (result.outcome.status == XR_XIR_CALL_READY);
        }
        if (mode != ORDER_FREE) {
            CHECK(result.outcome.status == XR_XIR_CALL_OUTPUT_ERROR && output->count == 3);
            CHECK(xr_xir_call_state(instance->call) == XR_XIR_CALL_OUTPUT_ERROR);
            CHECK(!xr_xir_call_cleanup_incomplete(instance->call));
            CHECK(xr_xir_task_executor_completion_status(instance->executor) == XR_XIR_CALL_OUTPUT_ERROR);
            CHECK(order_poll(instance, output).outcome.status == XR_XIR_CALL_OUTPUT_ERROR);
        }
        XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domain);
        if (mode == ORDER_RETRY) {
            uint64_t epoch = instance->epoch; XrXirCall *old = instance->call;
            CHECK(xr_xir_instance_start(instance, UINT32_MAX, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
            CHECK(instance->epoch == epoch && instance->call == old);
            CHECK(order_poll(instance, output).outcome.status == XR_XIR_CALL_OUTPUT_ERROR);
            CHECK(instance->executor->first_failure == XR_XIR_CALL_OUTPUT_ERROR);
            XrXirDomainBudgetStats rejected = xr_xir_domain_budget_stats(domain);
            CHECK(before.requested_bytes == rejected.requested_bytes &&
                before.requested_call_bytes == rejected.requested_call_bytes && before.work == rejected.work);
            CHECK(xr_xir_instance_start(instance, 4, NULL, 0) == XR_XIR_CALL_READY);
            CHECK(instance->epoch == epoch + 1 && instance->executor->first_failure == XR_XIR_CALL_READY);
            CHECK(instance->executor->shutdown_status == XR_XIR_CALL_READY);
            do { result = order_poll(instance, output); } while (result.outcome.status == XR_XIR_CALL_READY);
            CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && output->count == 3);
            XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
            CHECK(value.type == XR_XIR_I64 && !value.reserved && value.payload == 7); xr_xir_value_drop(&value);
            XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(domain);
            CHECK(after.requested_bytes >= before.requested_bytes && after.requested_call_bytes > before.requested_call_bytes &&
                after.work > before.work);
        }
        XrXirCallStatus freed = xr_xir_instance_free(instance);
        CHECK(freed == (mode == ORDER_RETRY ? XR_XIR_CALL_READY : XR_XIR_CALL_OUTPUT_ERROR));
        CHECK(output->count == 3 && output->steps[1] < output->steps[2]);
        XrXirDomainBudgetStats fees = xr_xir_domain_budget_stats(domain); xr_xir_domain_drop(domain);
        printf("{\"mode\":%u,\"instance\":%u,\"accepted_root13\":%u,\"terminal_child6\":%u,\"cleanup_output6\":%u,\"free\":%u,\"requested_value\":%llu,\"requested_call\":%llu,\"work\":%llu}\n",
            mode, i, accepted, child_terminal, output->steps[2], freed,
            (unsigned long long)fees.requested_bytes, (unsigned long long)fees.requested_call_bytes, (unsigned long long)fees.work);
    }
    CHECK(!runtime_live && !runtime_bytes);
    CHECK(library_compile_stats(&owner.context).live_bytes == owner.baseline.live_bytes);
    library_compile_owner_drop(&owner);
    CHECK(!source_program_compile_live && !source_program_compile_bytes);
    printf("{\"mode\":%u,\"instances\":2,\"compiler_physical_zero\":true,\"runtime_physical_zero\":true}\n", mode);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    for (uint32_t mode = 0; mode < ORDER_MODE_COUNT; ++mode) order_execute(mode);
    library_compile_observer_free(); return 0;
}
