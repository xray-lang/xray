/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source17_native_resources.c - Genuine Source programs with isolated runtime owners
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_go_source_native_compile_owner.h"
#include "xir_go_source_native_allocations.h"
#include "xir/xxir_task.c"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_format.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_specialize.c"
#include "xir_go_source_native_oracles.h"
#include "source17_generated.h"
static XrXirInstance **source_native_instances;
static void source_native_event_role(SourceNativeAllocationEvent *event) {
    if (!source_native_instances || event->instance >= 2) return;
    XrXirInstance *instance = source_native_instances[event->instance];
    if (!instance || !instance->executor) return;
    XrXirTaskExecutor *executor = instance->executor;
    XrXirCall *call = executor->current ? executor->current->call : instance->call;
    if (!call) return;
    event->role = executor->current && executor->current->task ? 2u : 1u;
    if (call->top) event->entry = (unsigned)(call->top->entry - call->config.entries);
}
typedef struct SourceNativeCost {
    size_t sites, stages[10];
    uint32_t polls[2];
    XrXirCallStatus primary[2], disposed[2], failure;
    XrXirDomainBudgetStats budget[2];
    XrXirDomainStats values[2];
    bool hit;
    size_t request_before, request_after, post_request_sites, disposed_sites;
    bool requested;
    XrXirCallStatus request_status;
} SourceNativeCost;
static XrXirCallStatus source_native_value_status(XrXirValueStatus status) {
    CHECK(status == XR_XIR_VALUE_OK || status == XR_XIR_VALUE_OOM || status == XR_XIR_VALUE_LIMIT);
    return status == XR_XIR_VALUE_OK ? XR_XIR_CALL_READY :
        status == XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_LIMIT;
}
static void source_native_failure(XrXirInstance *instance, XrXirCallStatus status, uint32_t entry) {
    XrXirValue output = {0};
    if (xr_xir_instance_state(instance) == XR_XIR_INSTANCE_FAILED) {
        XrXirCallResult first = {0}, second = {0};
        CHECK(xr_xir_instance_copy_failure(instance, &first) == status);
        CHECK(xr_xir_instance_copy_failure(instance, &second) == status);
        CHECK(!memcmp(&first, &second, sizeof(first)));
        CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == status);
        CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == status);
        CHECK(xr_xir_instance_take_result(instance, &output) == XR_XIR_CALL_BAD_STATE);
        xr_xir_call_result_drop(&first); xr_xir_call_result_drop(&second);
    } else {
        XrXirCallStatus completed = xr_xir_task_executor_completion_status(instance->executor);
        XrXirCallStatus expected = completed == XR_XIR_CALL_READY ? XR_XIR_CALL_BAD_STATE : completed;
        CHECK(xr_xir_instance_take_result(instance, &output) == expected);
    }
    CHECK(!output.type && !output.reserved && !output.payload);
}
static SourceNativeCost source_native_operation(unsigned index, size_t fault,
    const XrXirInstanceConfig *limits, uint32_t cancel_prefix, unsigned cancel_instance) {
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    SourceNativeCost cost = {0}; XrXirProgram *program = NULL;
    const SourceTaskOracle *oracle = &source_task_oracles[index];
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    CHECK(xr_xir_compile_program_seal(context, source17_specs[index], &program) == XR_XIR_OK);
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (limits) config = *limits;
    XrXirInstance *instances[2] = {0}; XrXirCallResult owned[2] = {0};
    XrXirDomain *domains[2] = {0};
    CHECK(!runtime_live && !runtime_bytes);
    runtime_attempts = 0; runtime_fail_at = fault;
    source_native_instances = instances; source_native_event_count = 0; source_native_recording = true;
    for (unsigned pass = 0; pass < 2; ++pass) {
        source_native_phase = 0; source_native_instance = pass; source_native_poll = 0;
        cost.primary[pass] = xr_xir_instance_new(program, &config, &instances[pass]);
        if (cost.primary[pass] != XR_XIR_CALL_READY) {
            CHECK(!instances[pass]); cost.failure = cost.primary[pass]; goto done;
        }
        domains[pass] = instances[pass]->domain; CHECK(xr_xir_domain_retain(domains[pass]));
    }
    CHECK(instances[0]->domain != instances[1]->domain);
    cost.stages[0] = runtime_attempts;
    xr_xir_compile_program_drop(program); program = NULL;
    for (unsigned pass = 0; pass < 2; ++pass) {
        XrXirInstance *instance = instances[pass];
        source_native_phase = 1; source_native_instance = pass;
        cost.primary[pass] = xr_xir_instance_start(instance, source17_entries[index], NULL, 0);
        cost.stages[1 + 4 * pass] = runtime_attempts;
        if (cost.primary[pass] != XR_XIR_CALL_READY) {
            cost.failure = cost.primary[pass]; goto done;
        }
        XrXirInstanceResult result = {0};
        do {
            source_native_poll = cost.polls[pass];
            if (pass == cancel_instance && cost.polls[pass] == cancel_prefix) {
                source_native_phase = 6; cost.request_before = runtime_attempts;
                cost.request_status = xr_xir_instance_cancel_current(instance);
                CHECK(cost.request_status == XR_XIR_CALL_CANCEL_REQUESTED);
                cost.request_after = runtime_attempts; cost.requested = true;
                CHECK(cost.request_after == cost.request_before);
            }
            source_native_phase = 2;
            result = xr_xir_instance_poll_bounded(instance, 1);
            CHECK(++cost.polls[pass] < 32768);
        } while (result.outcome.status == XR_XIR_CALL_READY);
        cost.primary[pass] = result.outcome.status; cost.stages[2 + 4 * pass] = runtime_attempts;
        if (cost.primary[pass] == XR_XIR_CALL_OOM || cost.primary[pass] == XR_XIR_CALL_LIMIT ||
            cost.primary[pass] == XR_XIR_CALL_CANCELLED) {
            cost.failure = cost.primary[pass];
            source_native_phase = 7; source_native_failure(instance, cost.primary[pass], source17_entries[index]);
            goto done;
        }
        source_task_assert(oracle, &result.outcome);
        source_native_phase = 3;
        cost.failure = source_native_value_status(xr_xir_call_result_copy(&result.outcome, &owned[pass]));
        cost.stages[3 + 4 * pass] = runtime_attempts;
        if (cost.failure != XR_XIR_CALL_READY) { CHECK(xr_xir_call_result_empty(&owned[pass])); goto done; }
        cost.budget[pass] = xr_xir_domain_budget_stats(instance->domain);
        cost.values[pass] = xr_xir_domain_stats(instance->domain);
        source_native_phase = 4; source_native_instance = pass;
        cost.disposed[pass] = xr_xir_instance_free(instance); instances[pass] = NULL;
        cost.stages[4 + 4 * pass] = runtime_attempts;
        if (cost.disposed[pass] != XR_XIR_CALL_READY) { cost.failure = cost.disposed[pass]; goto done; }
    }
 done:
    for (unsigned pass = 0; pass < 2; ++pass) if (instances[pass]) {
        cost.budget[pass] = xr_xir_domain_budget_stats(instances[pass]->domain);
        cost.values[pass] = xr_xir_domain_stats(instances[pass]->domain);
        source_native_phase = 4; source_native_instance = pass;
        cost.disposed[pass] = xr_xir_instance_free(instances[pass]); instances[pass] = NULL;
        if (cost.failure == XR_XIR_CALL_READY && cost.disposed[pass] != XR_XIR_CALL_READY)
            cost.failure = cost.disposed[pass];
    }
    cost.disposed_sites = runtime_attempts; source_native_phase = 8;
    xr_xir_compile_program_drop(program);
    /* Finish the observation lease before proving the escaped result's own lifetime. */
    for (unsigned pass = 0; pass < 2; ++pass) if (domains[pass]) {
        cost.budget[pass] = xr_xir_domain_budget_stats(domains[pass]);
        cost.values[pass] = xr_xir_domain_stats(domains[pass]);
        xr_xir_domain_drop(domains[pass]); domains[pass] = NULL;
    }
    for (unsigned pass = 0; pass < 2; ++pass)
        if (owned[pass].status != XR_XIR_CALL_READY) source_task_assert(oracle, &owned[pass]);
    /* Borrow assertions and physical result drops spend no additional domain work. */
    source_native_phase = 9;
    xr_xir_call_result_drop(&owned[0]); xr_xir_call_result_drop(&owned[1]);
    cost.stages[9] = runtime_attempts; cost.sites = runtime_attempts;
    if (cost.requested) cost.post_request_sites = cost.sites - cost.request_after;
    CHECK(source_native_event_count == cost.sites);
    source_native_recording = false; source_native_instances = NULL;
    cost.hit = fault != SIZE_MAX && runtime_attempts > fault; runtime_fail_at = SIZE_MAX;
    CHECK(!runtime_live && !runtime_bytes);
    effects_source_owners_free(); CHECK(!effects_compile_live && !effects_compile_bytes);
    return cost;
}
static void source_native_print(unsigned index, const SourceNativeCost *cost) {
    printf("NATIVE_BASE %s sites=%zu primary=%u/%u polls=%u/%u failure=%u physical=0/0\n",
        source_task_oracles[index].name, cost->sites, cost->primary[0], cost->primary[1],
        cost->polls[0], cost->polls[1], cost->failure);
    for (unsigned pass = 0; pass < 2; ++pass)
        printf("NATIVE_INSTANCE pass=%u requested_value=%llu requested_call=%llu work=%llu metadata_peak=%llu value_peak=%llu call_peak=%llu\n",
            pass, (unsigned long long)cost->budget[pass].requested_bytes,
            (unsigned long long)cost->budget[pass].requested_call_bytes, (unsigned long long)cost->budget[pass].work,
            (unsigned long long)cost->budget[pass].metadata_peak, (unsigned long long)cost->values[pass].peak_bytes,
            (unsigned long long)cost->budget[pass].call_peak);
    for (unsigned stage = 0; stage < 10; ++stage)
        printf("NATIVE_STAGE stage=%u sites=%zu\n", stage, cost->stages[stage]);
}
static void source_native_faults(unsigned index, const SourceNativeCost *base) {
    for (size_t ordinal = 0; ordinal < base->sites; ++ordinal) {
        SourceNativeCost cost = source_native_operation(index, ordinal, NULL, UINT32_MAX, UINT32_MAX);
        CHECK(cost.hit && cost.failure == XR_XIR_CALL_OOM);
        /* Unseen allocations on a failed branch require a separate fault census. */
        printf("NATIVE_FAILURE_CENSUS ordinal=%zu attempts=%zu\n", ordinal, cost.sites);
        CHECK(cost.sites == ordinal + 1);
        printf("NATIVE_FAULT %s ordinal=%zu/%zu hit=1 status=%u attempts=%zu physical=0/0\n",
            source_task_oracles[index].name, ordinal, base->sites, cost.failure, cost.sites);
    }
}
static void source_native_axes(unsigned index, const SourceNativeCost *base) {
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrXirInstanceConfig config;
        CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        uint64_t exact = axis == 0 ? base->budget[0].requested_call_bytes :
            axis == 1 ? base->budget[0].metadata_peak : base->budget[0].work;
        CHECK(exact);
        if (!axis) config.requested_call_limit = exact - minus;
        else if (axis == 1) config.metadata_limit = exact - minus;
        else config.work_limit = exact - minus;
        SourceNativeCost cost = source_native_operation(index, SIZE_MAX, &config, UINT32_MAX, UINT32_MAX);
        CHECK(cost.failure == (minus ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY));
        printf("NATIVE_AXIS %s axis=%u minus=%u cut=%llu failure=%u physical=0/0\n",
            source_task_oracles[index].name, axis, minus, (unsigned long long)(exact - minus), cost.failure);
    }
}
static SourceNativeAllocationEvent source_native_normal[32768], source_native_cancel_trace[32768];
static bool source_native_same_event(const SourceNativeAllocationEvent *left,
    const SourceNativeAllocationEvent *right) {
    return left->ordinal == right->ordinal && left->bytes == right->bytes && left->line == right->line &&
        left->kind == right->kind && left->phase == right->phase && left->instance == right->instance &&
        left->poll == right->poll && left->role == right->role && left->entry == right->entry &&
        !strcmp(left->file, right->file);
}
static void source_native_print_events(const char *name, unsigned pass, uint32_t prefix) {
    for (size_t i = 0; i < source_native_event_count; ++i) {
        const SourceNativeAllocationEvent *event = &source_native_events[i];
        const char *file = strrchr(event->file, '/'), *back = strrchr(event->file, '\\');
        if (back && (!file || back > file)) file = back;
        file = file ? file + 1 : event->file;
        printf("NATIVE_ALLOC %s cancel_instance=%u prefix=%u ordinal=%zu bytes=%zu kind=%u file=%s line=%u phase=%u instance=%u poll=%u role=%u entry=%u hit=%u\n",
            name, pass, prefix, event->ordinal, event->bytes, event->kind, file, event->line, event->phase,
            event->instance, event->poll, event->role, event->entry, (unsigned)event->injected);
    }
}
static void source_native_cancel_one(unsigned index, unsigned pass, uint32_t prefix,
    const SourceNativeCost *base, bool scan_late) {
    SourceNativeCost cost = source_native_operation(index, SIZE_MAX, NULL, prefix, pass);
    CHECK(cost.requested && cost.failure == XR_XIR_CALL_CANCELLED && !cost.hit);
    CHECK(cost.request_before == cost.request_after && cost.request_before <= base->sites);
    for (size_t i = 0; i < cost.request_before; ++i)
        CHECK(source_native_same_event(&source_native_events[i], &source_native_normal[i]));
    memcpy(source_native_cancel_trace, source_native_events, cost.sites * sizeof(*source_native_events));
    printf("NATIVE_CANCEL %s instance=%u prefix=%u/%u request_status=%u before=%zu after=%zu late=%zu disposed_sites=%zu failure=%u primary=%u/%u free=%u/%u attempts=%zu physical=0/0\n",
        source_task_oracles[index].name, pass, prefix, base->polls[pass], cost.request_status,
        cost.request_before, cost.request_after, cost.post_request_sites, cost.disposed_sites,
        cost.failure, cost.primary[0], cost.primary[1], cost.disposed[0], cost.disposed[1], cost.sites);
    source_native_print_events(source_task_oracles[index].name, pass, prefix);
    if (!scan_late) return;
    /* Each cancellation history independently owns all observed post-request allocation points. */
    for (size_t ordinal = cost.request_after; ordinal < cost.sites; ++ordinal) {
        SourceNativeCost failed = source_native_operation(index, ordinal, NULL, prefix, pass);
        CHECK(failed.requested && failed.hit && failed.failure == XR_XIR_CALL_OOM);
        /* Do not certify a newly allocating failure suffix without testing it. */
        printf("NATIVE_FAILURE_CENSUS ordinal=%zu attempts=%zu\n", ordinal, failed.sites);
        CHECK(failed.sites == ordinal + 1);
        CHECK(source_native_event_count > ordinal && source_native_events[ordinal].injected);
        for (size_t i = 0; i <= ordinal; ++i)
            CHECK(source_native_same_event(&source_native_events[i], &source_native_cancel_trace[i]));
        printf("NATIVE_CANCEL_FAULT %s instance=%u prefix=%u ordinal=%zu/%zu request_before=%zu request_after=%zu hit=1 status=%u attempts=%zu physical=0/0\n",
            source_task_oracles[index].name, pass, prefix, ordinal, cost.sites,
            failed.request_before, failed.request_after, failed.failure, failed.sites);
        source_native_print_events(source_task_oracles[index].name, pass, prefix);
    }
}
static void source_native_cancel(unsigned index, const SourceNativeCost *base) {
    for (unsigned pass = 0; pass < 2; ++pass) for (uint32_t prefix = 0; prefix < base->polls[pass]; ++prefix)
        source_native_cancel_one(index, pass, prefix, base, true);
}
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4 || argc == 5); setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(!strcmp(argv[1], "--measure") || !strcmp(argv[1], "--faults") ||
        !strcmp(argv[1], "--axes") || !strcmp(argv[1], "--cancel") ||
        !strcmp(argv[1], "--probe-cancel") || !strcmp(argv[1], "--probe-fault"));
    unsigned matched = 0;
    for (unsigned i = 0; i < sizeof(source_task_oracles) / sizeof(*source_task_oracles); ++i)
        if (!strcmp(argv[2], source_task_oracles[i].name)) {
            SourceNativeCost base = source_native_operation(i, SIZE_MAX, NULL, UINT32_MAX, UINT32_MAX);
            CHECK(base.failure == XR_XIR_CALL_READY && base.sites && base.primary[0] == source_task_oracles[i].status &&
                base.primary[1] == source_task_oracles[i].status);
            source_native_print(i, &base);
            memcpy(source_native_normal, source_native_events, base.sites * sizeof(*source_native_events));
            source_native_print_events(source_task_oracles[i].name, UINT32_MAX, UINT32_MAX);
            if (!strcmp(argv[1], "--faults")) source_native_faults(i, &base);
            if (!strcmp(argv[1], "--axes")) source_native_axes(i, &base);
            if (!strcmp(argv[1], "--cancel")) source_native_cancel(i, &base);
            if (!strcmp(argv[1], "--probe-cancel")) {
                CHECK(argc == 5); unsigned pass = (unsigned)strtoul(argv[3], NULL, 10);
                uint32_t prefix = (uint32_t)strtoul(argv[4], NULL, 10);
                CHECK(pass < 2 && prefix < base.polls[pass]); source_native_cancel_one(i, pass, prefix, &base, false);
            } else if (!strcmp(argv[1], "--probe-fault")) {
                CHECK(argc == 4); size_t ordinal = (size_t)strtoull(argv[3], NULL, 10); CHECK(ordinal < base.sites);
                SourceNativeCost failed = source_native_operation(i, ordinal, NULL, UINT32_MAX, UINT32_MAX);
                CHECK(failed.hit && failed.failure == XR_XIR_CALL_OOM && failed.sites == ordinal + 1);
                printf("NATIVE_PROBE_FAULT %s ordinal=%zu hit=1 status=%u attempts=%zu physical=0/0\n",
                    source_task_oracles[i].name, ordinal, failed.failure, failed.sites);
                source_native_print_events(source_task_oracles[i].name, UINT32_MAX, UINT32_MAX);
            } else CHECK(argc == 3);
            ++matched;
        }
    CHECK(matched == 1); return 0;
}
