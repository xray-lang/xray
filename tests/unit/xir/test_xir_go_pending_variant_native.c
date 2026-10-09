/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_pending_exit_variants_native.c - First fault, cancellation and stream bool
 */
#include "xir_construction_fixture.h"
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
#include "xir_pending_exit_variants_fixture.h"
XR_DATA const XrXirProgramSpec pending_variant_body_program;
XR_DATA const XrXirProgramSpec pending_variant_nested_program;
XR_DATA const XrXirProgramSpec pending_variant_write_program;
typedef struct VariantLog {
    XrXirInstance *instance;
    int64_t trace[16];
    unsigned count, kind;
    bool rejected, cancellation, malformed;
    XrXirCallStatus request;
} VariantLog;
static XrXirOutputStatus variant_output(void *context, const XrXirOutputGroup *group) {
    VariantLog *log = context;
    if (!group || group->stream != XR_XIR_STDOUT || group->line || group->count != 1 ||
        !group->values || group->values[0].reserved || log->count >= 16) {
        log->malformed = true; return XR_XIR_OUTPUT_ERROR;
    }
    int64_t value;
    if (group->values[0].type == XR_XIR_STRING && log->kind == 2) {
        const char *bytes = NULL; size_t length = 0;
        if (!xr_xir_string_view(&group->values[0], &bytes, &length) || length != 1 || bytes[0] != '7') {
            log->malformed = true; return XR_XIR_OUTPUT_ERROR;
        }
        value = 7;
    } else if (group->values[0].type == XR_XIR_I64) value = group->values[0].payload;
    else { log->malformed = true; return XR_XIR_OUTPUT_ERROR; }
    log->trace[log->count++] = value;
    if ((((log->kind == 3 || log->kind == 8) && value == 7) || ((log->kind == 7 || log->kind == 9) && value == 41)) && !log->cancellation) {
        if (log->kind == 3 || log->kind == 7) {
            log->request = xr_xir_call_request_cancel(log->instance->call);
        } else log->request = xr_xir_instance_cancel_current(log->instance);
        fprintf(stderr, "callback-cancel kind=%u api=%s status=%u instance_driving=%u executor_driving=%u call_driving=%u root_same=%u current_same=%u requested=%u\n",
            log->kind, log->kind == 3 || log->kind == 7 ? "Call_request_cancel" : "Instance_cancel_current",
            log->request, (unsigned)log->instance->driving, (unsigned)log->instance->executor->driving,
            (unsigned)log->instance->call->driving,
            (unsigned)(log->instance->executor->root.call == log->instance->call),
            (unsigned)(log->instance->executor->current && log->instance->executor->current->call == log->instance->call),
            (unsigned)log->instance->call->cancel_requested);
        log->cancellation = true;
    }
    if (value == 42 && (log->kind == 5 || log->kind == 6)) return XR_XIR_OUTPUT_LIMIT;
    if (value == 7 && !log->rejected && log->kind != 6) { log->rejected = true; return XR_XIR_OUTPUT_ERROR; }
    return XR_XIR_OUTPUT_OK;
}
static bool variant_pending_fault(const XrXirCall *call) {
    for (const CallFrame *frame = call->top; frame; frame = frame->parent)
        if (frame->exiting && frame->pending.status == XR_XIR_CALL_OUTPUT_ERROR) return true;
    return false;
}
static XrXirInstanceResult variant_poll(VariantLog *log) {
    XrXirInstanceResult result = {0};
    for (unsigned n = 0; n < 3000; ++n) {
        result = xr_xir_instance_poll_bounded(log->instance, 1);
        if (log->kind == 4 && !log->cancellation && log->count == 1 &&
            result.outcome.status == XR_XIR_CALL_READY && variant_pending_fault(log->instance->call)) {
            log->request = xr_xir_instance_cancel_current(log->instance);
            log->cancellation = true;
        }
        if (result.outcome.status != XR_XIR_CALL_READY) return result;
    }
    CHECK(false); return result;
}
static XrXirProgram *variant_vm(const XrXirCompileContext *context, unsigned mode) {
    PendingExitVariantFixture fixture; pending_exit_variant_fixture(&fixture, mode);
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL, *lowered = NULL;
    XrXirCheckedPacket packet = {0}; XrXirProgram *program = NULL;
    CHECK(xir_fixture_check(context, &fixture.module, &checked, NULL) == XR_XIR_OK);
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
static void variant_case(unsigned kind, bool vm) {
    CHECK(kind < 10 && !runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    unsigned mode = kind == 1 || kind == 5 || kind == 6 ? 1 : kind == 2 ? 2 : 0;
    const XrCompileResourceLimits limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    XrXirCompileContext context = {.limits = xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline, final;
    CHECK(xr_compile_resources_stats(context.resources, &baseline) == XR_COMPILE_RESOURCE_OK);
    const XrXirProgramSpec *spec = mode == 1 ? &pending_variant_nested_program : mode == 2 ?
        &pending_variant_write_program : &pending_variant_body_program;
    XrXirProgram *program = NULL;
    if (vm) program = variant_vm(&context, mode);
    else CHECK(xr_xir_compile_program_seal(&context, spec, &program) == XR_XIR_OK);
    VariantLog log = {.kind = kind}; XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, variant_output, &log};
    CHECK(xr_xir_instance_new(program, &config, &log.instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(log.instance, 1, NULL, 0) == XR_XIR_CALL_READY);
    XrXirCallStatus primary = variant_poll(&log).outcome.status;
    bool incomplete = xr_xir_call_cleanup_incomplete(log.instance->call);
    unsigned first_count = log.count;
    XrXirValue value = {0};
    XrXirCallStatus taken = xr_xir_instance_take_result(log.instance, &value);
    bool first_value = value.type == XR_XIR_I64 && !value.reserved && value.payload == 7;
    xr_xir_value_drop(&value);
    XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(log.instance->domain);
    XrXirCallStatus restart = xr_xir_instance_start(log.instance, 1, NULL, 0), second = XR_XIR_CALL_READY;
    bool second_value = false;
    if (restart == XR_XIR_CALL_READY) {
        second = variant_poll(&log).outcome.status;
        taken = xr_xir_instance_take_result(log.instance, &value);
        second_value = taken == XR_XIR_CALL_RETURNED && value.type == XR_XIR_I64 && !value.reserved &&
            value.payload == (kind == 2 ? 70u : 7u);
        xr_xir_value_drop(&value);
    }
    XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(log.instance->domain);
    XrXirCallStatus freed = xr_xir_instance_free(log.instance);
    CHECK(!runtime_live && !runtime_bytes);
    CHECK(xr_compile_resources_stats(context.resources, &final) == XR_COMPILE_RESOURCE_OK);
    CHECK(final.live_bytes == baseline.live_bytes);
    xr_compile_resources_release(context.resources);
    CHECK(!effects_compile_live && !effects_compile_bytes);
    printf("variant vm=%u kind=%u primary=%u first_count=%u incomplete=%u request=%u restart=%u second=%u free=%u physical=0/0\n",
        (unsigned)vm, kind, primary, first_count, (unsigned)incomplete, log.request, restart, second, freed);
    XrXirCallStatus expected = kind == 2 ? XR_XIR_CALL_RETURNED : kind == 3 ? XR_XIR_CALL_CANCELLED :
        kind == 6 ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_OUTPUT_ERROR;
    CHECK(primary == expected && !log.malformed && incomplete == (kind == 5 || kind == 6));
    CHECK(freed == XR_XIR_CALL_READY);
    if (kind == 3 || kind == 4 || kind == 7) CHECK(log.cancellation && log.request == XR_XIR_CALL_CANCEL_REQUESTED);
    if (kind == 8 || kind == 9) CHECK(log.cancellation && log.request == XR_XIR_CALL_BUSY);
    if (kind == 5 || kind == 6) {
        CHECK(first_count == 2 && log.count == 2 && log.trace[0] == 7 && log.trace[1] == 42);
        CHECK(restart == XR_XIR_CALL_BAD_STATE);
    } else {
        unsigned stride = kind == 1 ? 3u : 2u;
        CHECK(first_count == stride && log.count == stride * 2 && restart == XR_XIR_CALL_READY &&
            second == XR_XIR_CALL_RETURNED && second_value && after.work > before.work &&
            after.requested_call_bytes > before.requested_call_bytes);
        for (unsigned i = 0; i < 2; ++i) {
            CHECK(log.trace[i * stride] == 7);
            CHECK(log.trace[i * stride + 1] == (kind == 1 ? 42 : 41));
            if (kind == 1) CHECK(log.trace[i * stride + 2] == 41);
        }
        if (kind == 2) CHECK(first_value);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 3); setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(!strcmp(argv[1], "vm") || !strcmp(argv[1], "native"));
    unsigned kind = (unsigned)strtoul(argv[2], NULL, 10);
    variant_case(kind, !strcmp(argv[1], "vm")); return 0;
}
