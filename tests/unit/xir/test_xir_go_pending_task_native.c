/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_pending_exit_task_native.c - Release first, then check fault cleanup
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
#include "xir_go_source_native_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_format.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_specialize.c"
#include "xir/xxir_vm.c"
#include "xir_task_fault_cleanup_fixture.h"
XR_DATA const XrXirProgramSpec pending_body_program;
XR_DATA const XrXirProgramSpec pending_call_program;
XR_DATA const XrXirProgramSpec pending_await_program;
typedef struct PendingLog { int64_t trace[16]; unsigned count; bool rejected, malformed; } PendingLog;
static XrXirOutputStatus pending_output(void *context, const XrXirOutputGroup *group) {
    PendingLog *log = context;
    if (!group || group->stream != XR_XIR_STDOUT || !group->line || group->count != 1 ||
        !group->values || group->values[0].type != XR_XIR_I64 || group->values[0].reserved || log->count >= 16) {
        log->malformed = true; return XR_XIR_OUTPUT_ERROR;
    }
    int64_t value = group->values[0].payload;
    log->trace[log->count++] = value;
    if (value == 7 && !log->rejected) { log->rejected = true; return XR_XIR_OUTPUT_ERROR; }
    return XR_XIR_OUTPUT_OK;
}
static XrXirInstanceResult pending_poll(XrXirInstance *instance) {
    XrXirInstanceResult result = {0};
    for (unsigned n = 0; n < 3000; ++n) {
        result = xr_xir_instance_poll_bounded(instance, 1);
        if (result.outcome.status != XR_XIR_CALL_READY) return result;
    }
    CHECK(false); return result;
}
static XrXirProgram *pending_vm(const XrXirCompileContext *context, unsigned mode) {
    TaskFaultFixture fixture; task_fault_fixture(&fixture, mode);
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0}; XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_check(context, &fixture.module, &checked, NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked); memset(&fixture, 0xa5, sizeof(fixture));
    CHECK(xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);
    XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK && !lowered);
    return program;
}
typedef struct PendingFacts {
    XrXirCallStatus primary, occupied, empty, restart, second, free_status;
    bool incomplete, retained_output, physical, cumulative;
    PendingLog output;
} PendingFacts;
static PendingFacts pending_execute(XrXirInstance *instance, PendingLog *output) {
    PendingFacts facts = {0};
    CHECK(xr_xir_instance_start(instance, 0, NULL, 0) == XR_XIR_CALL_READY);
    facts.primary = pending_poll(instance).outcome.status;
    facts.incomplete = instance->executor->cleanup_incomplete || xr_xir_call_cleanup_incomplete(instance->call);
    XrXirValue sentinel = {XR_XIR_I64, 0, 99}, empty = {0};
    facts.occupied = xr_xir_instance_take_result(instance, &sentinel);
    facts.empty = xr_xir_instance_take_result(instance, &empty);
    facts.retained_output = sentinel.type == XR_XIR_I64 && !sentinel.reserved && sentinel.payload == 99 &&
        !empty.type && !empty.reserved && !empty.payload;
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(instance->domain);
    facts.restart = xr_xir_instance_start(instance, 0, NULL, 0);
    if (facts.restart == XR_XIR_CALL_READY) {
        facts.second = pending_poll(instance).outcome.status;
        XrXirValue value = {0};
        XrXirCallStatus taken = xr_xir_instance_take_result(instance, &value);
        if (taken != XR_XIR_CALL_RETURNED || value.type != XR_XIR_I64 || value.reserved || value.payload != 7)
            output->malformed = true;
        xr_xir_value_drop(&value);
    }
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(instance->domain);
    facts.cumulative = after.work > before.work && after.requested_call_bytes > before.requested_call_bytes;
    facts.free_status = xr_xir_instance_free(instance);
    facts.output = *output;
    return facts;
}
static void pending_case(unsigned mode, bool vm) {
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    const XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    XrXirCompileContext context = {.limits = xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline, final;
    CHECK(xr_compile_resources_stats(context.resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    const XrXirProgramSpec *spec = mode == TASK_FAULT_BODY ? &pending_body_program :
        mode == TASK_FAULT_CALL ? &pending_call_program : &pending_await_program;
    XrXirProgram *program = NULL;
    if (vm) program = pending_vm(&context, mode);
    else CHECK(xr_xir_compile_program_seal(&context, spec, &program) == XR_XIR_OK);
    PendingLog outputs[2] = {0}; XrXirInstance *instances[2] = {0}; PendingFacts facts[2];
    for (unsigned i = 0; i < 2; ++i) {
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, pending_output, &outputs[i]};
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    }
    CHECK(instances[0]->domain != instances[1]->domain);
    xr_xir_compile_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) facts[i] = pending_execute(instances[i], &outputs[i]);
    CHECK(!runtime_live && !runtime_bytes);
    CHECK(xr_compile_resources_stats(context.resources, &final) == XR_COMPILE_RESOURCE_OK);
    CHECK(final.live_bytes == baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(!effects_compile_live && !effects_compile_bytes);
    for (unsigned i = 0; i < 2; ++i) {
        PendingFacts *f = &facts[i]; unsigned stride = mode == TASK_FAULT_BODY ? 2 : 3;
        printf("pending vm=%u mode=%u instance=%u primary=%u calls=%u incomplete=%u restart=%u second=%u free=%u physical=0/0\n",
            (unsigned)vm, mode, i, f->primary, f->output.count, (unsigned)f->incomplete, f->restart, f->second, f->free_status);
        CHECK(f->primary == XR_XIR_CALL_OUTPUT_ERROR && !f->incomplete && f->retained_output && !f->output.malformed);
        CHECK(f->occupied == (mode == TASK_FAULT_CALL ? XR_XIR_CALL_BAD_ARGUMENT : XR_XIR_CALL_OUTPUT_ERROR));
        CHECK(f->empty == (mode == TASK_FAULT_CALL ? XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_OUTPUT_ERROR));
        CHECK(f->restart == XR_XIR_CALL_READY && f->second == XR_XIR_CALL_RETURNED && f->free_status == XR_XIR_CALL_READY);
        CHECK(f->cumulative && f->output.count == stride * 2);
        for (unsigned n = 0; n < 2; ++n) {
            CHECK(f->output.trace[n * stride] == 7 && f->output.trace[n * stride + 1] == 6);
            if (stride == 3) CHECK(f->output.trace[n * stride + 2] == 5);
        }
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 3); setvbuf(stdout, NULL, _IONBF, 0);
    unsigned mode = !strcmp(argv[2], "body") ? TASK_FAULT_BODY : !strcmp(argv[2], "call") ? TASK_FAULT_CALL : TASK_FAULT_AWAIT;
    CHECK(!strcmp(argv[2], "body") || !strcmp(argv[2], "call") || !strcmp(argv[2], "await"));
    CHECK(!strcmp(argv[1], "vm") || !strcmp(argv[1], "native"));
    pending_case(mode, !strcmp(argv[1], "vm")); return 0;
}
