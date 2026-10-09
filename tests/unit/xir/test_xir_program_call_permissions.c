/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_program_call_permissions.c - Reject child actions before target entry
 *
 * KEY CONCEPT:
 *   Authentic child requests expose entry admission separately from slot guards.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "PCE FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_program_call_permissions_fixture.h"
#include "xir_program_call_permissions_observer.h"

static XrXirInstanceResult pce_drive(PceWitness *w) {
    XrXirInstanceResult result = {0};
    unsigned polls = 0;
    do {
        result = xr_xir_instance_poll_bounded(w->instance, 1);
        ++polls;
        if (w->boundary_captured && !w->boundary_checked) {
            w->after_call = pce_ledger(w);
            w->fee_preserved = pce_same_fee(&w->before_call, &w->after_call);
            w->slot_preserved = !memcmp(&w->slot_before, &w->instance->slots[0], sizeof(w->slot_before));
            w->boundary_checked = true;
            pce_print_ledger(w, "before_action_accept", &w->before_call);
            pce_print_ledger(w, "after_one_quantum", &w->after_call);
        }
    } while (result.outcome.status == XR_XIR_CALL_READY && polls < 1024);
    printf("PCE_POLL instance=%u status=%u state=%u epoch=%llu polls=%u\n", w->index,
        result.outcome.status, xr_xir_instance_state(w->instance), (unsigned long long)result.epoch, polls);
    return result;
}

static void pce_initialize(PceFixture *f, PceWitness *w) {
    CHECK(xr_xir_instance_state(w->instance) == XR_XIR_INSTANCE_NEW);
    CHECK(xr_xir_instance_start(w->instance, f->entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult result = pce_drive(w);
    CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && result.epoch);
    CHECK(result.outcome.value.type == XR_XIR_I64 && !result.outcome.value.reserved && !result.outcome.value.payload);
    CHECK(xr_xir_instance_state(w->instance) == XR_XIR_INSTANCE_READY);
    CHECK(w->init_entered && w->module_begins == 1 && w->module_ready == 1 && w->published == 1);
    CHECK(w->entry_releases[f->entry] == 1 && !w->entry_releases[f->entries[PCE_MAIN]]);
    CHECK(!w->child_callbacks && !w->call_requests && !w->target_entered && !w->typed_groups);
    CHECK(w->instance->slots[0].type == XR_XIR_I64 && !w->instance->slots[0].reserved &&
        w->instance->slots[0].payload == 91);
    XrXirValue taken = {0};
    CHECK(xr_xir_instance_take_result(w->instance, &taken) == XR_XIR_CALL_RETURNED);
    CHECK(taken.type == XR_XIR_I64 && !taken.reserved && !taken.payload);
    size_t attempts = runtime_attempts;
    xr_xir_value_drop(&taken);
    CHECK(!taken.type && !taken.reserved && !taken.payload && attempts == runtime_attempts);
    printf("PCE_INIT instance=%u synthetic_entry=%u script_entry=%u declared_main=%u initializer=%u entered=%u state_i64=91 result_i64=0\n",
        w->index, f->functions, f->entry, f->entries[PCE_MAIN], f->initializer, w->init_entered);
}

static void pce_run_child(PceFixture *f, PceWitness *w) {
    CHECK(xr_xir_instance_start(w->instance, f->entries[PCE_RUN], NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult result = pce_drive(w);
    w->outcome = result.outcome.status;
    w->finished_state = xr_xir_instance_state(w->instance);
    w->borrowed = result.outcome.value; /* I64/Unit snapshots carry no owned lease. */
    w->terminal = pce_ledger(w);
    pce_print_ledger(w, "terminal", &w->terminal);
    XrXirValue occupied = {XR_XIR_I64, 0, INT64_C(112233)};
    const XrXirValue saved = occupied;
    w->take_occupied = xr_xir_instance_take_result(w->instance, &occupied);
    w->occupied_preserved = !memcmp(&occupied, &saved, sizeof(occupied));
    XrXirValue empty = {0};
    w->take_empty = xr_xir_instance_take_result(w->instance, &empty);
    w->empty_preserved = !empty.type && !empty.reserved && !empty.payload;
    w->taken = empty;
    xr_xir_value_drop(&empty);
    CHECK(!empty.type && !empty.reserved && !empty.payload);
    printf("PCE_OBSERVED instance=%u request=%u child_callbacks=%u target_entered=%u leaf_entered=%u "
        "typed_groups=%u byte_writes=%u bytes=%zu boundary_checked=%u fee_preserved=%u "
        "slot_preserved=%u status=%u take_empty=%u take_occupied=%u\n", w->index, w->call_requests,
        w->child_callbacks, w->target_entered, w->leaf_entered, w->typed_groups, w->byte_writes,
        w->length, (unsigned)w->boundary_checked, (unsigned)w->fee_preserved, (unsigned)w->slot_preserved,
        w->outcome, w->take_empty, w->take_occupied);
}

static void pce_drop_advertised(PceFixture *f, PceWitness *w) {
    if (f->mode != PCE_ADVERTISED_UNKNOWN) {
        CHECK(!w->carrier_created && !w->advertised.type && !w->advertised.reserved && !w->advertised.payload);
        return;
    }
    CHECK(w->carrier_created == 1 && !w->carrier_dropped && w->advertised.type == (uint32_t)f->advertised_type);
    size_t attempts = runtime_attempts;
    xr_xir_value_drop(&w->advertised);
    CHECK(!w->advertised.type && !w->advertised.reserved && !w->advertised.payload && runtime_attempts == attempts);
    ++w->carrier_dropped;
    printf("PCE_CARRIER instance=%u phase=drop legitimate_owned_lease=1 new_malloc=0\n", w->index);
}

static void pce_peer_unchanged(PceWitness *peer, const PceLedger *before) {
    PceLedger after = pce_ledger(peer);
    /* Global provider attempts include the active peer; this owner's fees do not. */
    after.runtime_attempts = before->runtime_attempts;
    CHECK(pce_same_fee(before, &after));
    CHECK(peer->instance->slots[0].type == XR_XIR_I64 && !peer->instance->slots[0].reserved &&
        peer->instance->slots[0].payload == 91);
}

static void pce_close_pair(PceFixture *f) {
    for (unsigned i = 0; i < PCE_INSTANCES; ++i) {
        PceWitness *w = &f->witnesses[i];
        w->stop = xr_xir_instance_stop(w->instance);
        w->freed = xr_xir_instance_free(w->instance);
        w->instance = NULL;
        CHECK(w->freed != XR_XIR_CALL_BUSY && w->released == 1);
        XrXirDomainBudgetStats fees = xr_xir_domain_budget_stats(w->domain);
        XrXirDomainStats physical = xr_xir_domain_stats(w->domain);
        CHECK(!fees.metadata_live && !fees.call_live);
        CHECK(fees.metadata_allocations == fees.metadata_frees && fees.call_allocations == fees.call_frees);
        CHECK(physical.live_bytes == sizeof(*w->domain) && physical.allocations == physical.frees + 1);
        CHECK(fees.requested_bytes >= w->terminal.domain.requested_bytes &&
            fees.requested_call_bytes >= w->terminal.domain.requested_call_bytes && fees.work >= w->terminal.domain.work);
        printf("PCE_CLOSE instance=%u stop=%u free=%u slot_releases=%u metadata_live=0 call_live=0 "
            "retained_domain_bytes=%llu value_allocations=%llu value_frees=%llu work=%llu\n",
            w->index, w->stop, w->freed, w->released, (unsigned long long)physical.live_bytes,
            (unsigned long long)physical.allocations, (unsigned long long)physical.frees,
            (unsigned long long)fees.work);
        size_t attempts = runtime_attempts;
        xr_xir_domain_drop(w->domain);
        w->domain = NULL;
        CHECK(runtime_attempts == attempts);
    }
    CHECK(!f->code_releases && f->lowered);
    xr_xir_compile_program_drop(f->program);
    f->program = NULL;
    CHECK(f->code_releases == 1 && !f->lowered);
    XrCompileResourceStats after = {0};
    CHECK(xr_compile_resources_stats(f->compiler.resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes == f->compiler_baseline.live_bytes);
    xr_compile_resources_release(f->compiler.resources);
    f->compiler.resources = NULL;
    pce_current = NULL;
    instance_compile_zero();
    if (runtime_live || runtime_bytes) runtime_report_residuals(stderr);
    CHECK(!runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    printf("PCE_PHYSICAL domains=2 code_releases=1 compiler_blocks=0 compiler_bytes=0 runtime_blocks=0 runtime_bytes=0\n");
}

static void pce_expect(const PceFixture *f) {
    for (unsigned i = 0; i < PCE_INSTANCES; ++i) {
        const PceWitness *w = &f->witnesses[i];
        CHECK(w->boundary_captured && w->boundary_checked && w->slot_preserved);
        CHECK(w->call_requests == 1 && w->child_callbacks && w->module_begins == 1 && w->module_ready == 1);
        CHECK(w->published == 1 && w->released == 1 && w->occupied_preserved);
        CHECK(w->finished_state == XR_XIR_INSTANCE_READY);
        CHECK(w->entry_releases[f->entries[PCE_WORKER]] == 1);
        CHECK(!w->advertised.type && !w->advertised.reserved && !w->advertised.payload);
        CHECK(w->carrier_created == (f->mode == PCE_ADVERTISED_UNKNOWN ? 1u : 0u));
        CHECK(w->carrier_dropped == w->carrier_created);
        if (f->mode == PCE_NORMAL) {
            CHECK(w->outcome == XR_XIR_CALL_RETURNED && w->take_empty == XR_XIR_CALL_RETURNED);
            CHECK(w->take_occupied == XR_XIR_CALL_BAD_ARGUMENT && w->stop == XR_XIR_CALL_READY &&
                w->freed == XR_XIR_CALL_READY);
            CHECK(w->borrowed.type == XR_XIR_I64 && !w->borrowed.reserved && w->borrowed.payload == 42);
            CHECK(w->taken.type == XR_XIR_I64 && !w->taken.reserved && w->taken.payload == 42);
            CHECK(w->target_entered && w->leaf_entered && w->typed_groups == 1 && w->byte_writes == 1);
            CHECK(w->length == 3 && !memcmp(w->bytes, "42\n", 3));
            CHECK(w->entry_releases[f->target] == 1);
        } else {
            CHECK(w->outcome == XR_XIR_CALL_BAD_STATE && w->take_empty == XR_XIR_CALL_BAD_STATE &&
                w->take_occupied == XR_XIR_CALL_BAD_STATE);
            CHECK(w->stop == XR_XIR_CALL_BAD_STATE && w->freed == XR_XIR_CALL_BAD_STATE);
            CHECK(!w->target_entered && !w->leaf_entered && !w->typed_groups && !w->byte_writes && !w->length);
            CHECK(w->empty_preserved && !w->borrowed.type && !w->borrowed.reserved && !w->borrowed.payload);
            CHECK(!w->taken.type && !w->taken.reserved && !w->taken.payload);
            CHECK(w->fee_preserved && !w->entry_releases[f->target]);
        }
    }
}

static void pce_run(PceMode mode) {
    CHECK(!runtime_live && !runtime_bytes && !instance_compile_live && !instance_compile_bytes);
    CHECK(runtime_fail_at == SIZE_MAX && instance_compile_fail_at == SIZE_MAX);
    PceFixture fixture;
    pce_build(&fixture, mode);
    pce_new_pair(&fixture);
    for (unsigned i = 0; i < PCE_INSTANCES; ++i) {
        PceWitness *w = &fixture.witnesses[i], *peer = &fixture.witnesses[1 - i];
        PceLedger before = pce_ledger(peer);
        XrXirValue slot = peer->instance->slots[0];
        pce_initialize(&fixture, w);
        PceLedger after = pce_ledger(peer);
        after.runtime_attempts = before.runtime_attempts;
        CHECK(pce_same_fee(&before, &after) && !memcmp(&slot, &peer->instance->slots[0], sizeof(slot)));
    }
    for (unsigned i = 0; i < PCE_INSTANCES; ++i) {
        PceWitness *w = &fixture.witnesses[i], *peer = &fixture.witnesses[1 - i];
        PceLedger before = pce_ledger(peer);
        unsigned peer_typed_groups = peer->typed_groups;
        pce_run_child(&fixture, w);
        pce_drop_advertised(&fixture, w);
        pce_peer_unchanged(peer, &before);
        CHECK(peer->typed_groups == peer_typed_groups);
    }
    pce_close_pair(&fixture);
    pce_expect(&fixture); /* Permission failures are reported only after physical release. */
}

int main(int argc, char **argv) {
    static const char *const modes[] = {"normal", "required", "unknown", "mixed", "advertised_unknown"};
    CHECK(argc == 2);
    unsigned mode = 0;
    while (mode < 5 && strcmp(argv[1], modes[mode])) ++mode;
    CHECK(mode < 5);
    printf("PCE mode=%s provider=trusted_native_host_adapter body=VM resources=NOT_QUALIFIED\n", modes[mode]);
    pce_run((PceMode)mode);
    printf("PCE PASS mode=%s same_program=1 actual_instances=2 actual_children=2\n", modes[mode]);
    return 0;
}
