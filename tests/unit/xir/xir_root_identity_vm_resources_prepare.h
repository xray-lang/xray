/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_identity_vm_resources_prepare.h - Resource admission before VM entry
 *
 * KEY CONCEPT:
 *   A failed preparation retains its original lifetime ledger. Allocator
 *   injection ends before the unchanged VM and authority observers execute.
 */
#ifndef XIR_ROOT_IDENTITY_VM_RESOURCES_PREPARE_H
#define XIR_ROOT_IDENTITY_VM_RESOURCES_PREPARE_H

typedef struct RrPhysical {
    size_t runtime_blocks, runtime_bytes, compiler_blocks, compiler_bytes;
} RrPhysical;
typedef struct RrSnapshot {
    const void *instance_owner, *domain_owner, *budget_owner;
    XrXirDomainBudgetStats domain;
    XrXirDomainStats values;
    XrXirCallBudget call;
    uint64_t epoch, value_limit;
    size_t attempts;
    XrXirInstanceState state;
    bool prepared;
} RrSnapshot;
typedef struct RrCensus {
    size_t create_sites, prepare_sites;
    uint64_t created_requested, prepared_requested, created_work, prepared_work;
    uint64_t prepare_requested, prepare_work, call_bytes, segment_bytes, minimum_call_live;
} RrCensus;

static RrPhysical rr_physical(void) {
    return (RrPhysical){runtime_live, runtime_bytes, effects_compile_live, effects_compile_bytes};
}
static void rr_same_physical(RrPhysical before) {
    CHECK(before.runtime_blocks == runtime_live && before.runtime_bytes == runtime_bytes);
    CHECK(before.compiler_blocks == effects_compile_live && before.compiler_bytes == effects_compile_bytes);
}
static RrSnapshot rr_snapshot(const RiWitness *w) {
    CHECK(w && w->instance);
    const XrXirInstance *instance = w->instance;
    return (RrSnapshot){instance, instance->domain, &instance->budget,
        xr_xir_domain_budget_stats(instance->domain), xr_xir_domain_stats(instance->domain),
        instance->budget, instance->epoch, instance->config.value_limit, runtime_attempts, xr_xir_instance_state(instance),
        instance->call != NULL};
}
static void rr_log_snapshot(const char *phase, size_t point, XrXirCallStatus status, const RrSnapshot *s) {
    printf("RR phase=%s point=%zu status=%u instance=%p domain=%p budget=%p "
        "state=%u epoch=%llu prepared=%u attempts=%zu "
        "requested_value=%llu/%llu requested_call=%llu/%llu work=%llu/%llu "
        "value_limit=%llu value_live=%llu value_peak=%llu value_alloc=%llu value_free=%llu "
        "metadata_limit=%llu domain_call_limit=%llu call_byte_limit=%llu "
        "metadata_live=%llu metadata_peak=%llu metadata_alloc=%llu metadata_free=%llu "
        "domain_call_live=%llu domain_call_peak=%llu domain_call_alloc=%llu domain_call_free=%llu "
        "call_requested=%llu/%llu call_live=%llu call_peak=%llu call_alloc=%llu call_free=%llu "
        "exhausted=%u\n", phase, point, (unsigned)status, (void *)s->instance_owner,
        (void *)s->domain_owner, (void *)s->budget_owner, (unsigned)s->state,
        (unsigned long long)s->epoch, (unsigned)s->prepared, s->attempts,
        (unsigned long long)s->domain.requested_bytes, (unsigned long long)s->domain.requested_limit,
        (unsigned long long)s->domain.requested_call_bytes, (unsigned long long)s->domain.requested_call_limit,
        (unsigned long long)s->domain.work, (unsigned long long)s->domain.work_limit,
        (unsigned long long)s->value_limit, (unsigned long long)s->values.live_bytes, (unsigned long long)s->values.peak_bytes,
        (unsigned long long)s->values.allocations, (unsigned long long)s->values.frees,
        (unsigned long long)s->domain.metadata_limit, (unsigned long long)s->domain.call_limit,
        (unsigned long long)s->call.byte_limit,
        (unsigned long long)s->domain.metadata_live, (unsigned long long)s->domain.metadata_peak,
        (unsigned long long)s->domain.metadata_allocations, (unsigned long long)s->domain.metadata_frees,
        (unsigned long long)s->domain.call_live, (unsigned long long)s->domain.call_peak,
        (unsigned long long)s->domain.call_allocations, (unsigned long long)s->domain.call_frees,
        (unsigned long long)s->call.requested_bytes, (unsigned long long)s->call.requested_limit,
        (unsigned long long)s->call.live_bytes, (unsigned long long)s->call.peak_bytes,
        (unsigned long long)s->call.allocations, (unsigned long long)s->call.frees, (unsigned)s->call.exhausted);
}
static void rr_same_owner(const RrSnapshot *before, const RrSnapshot *after) {
    CHECK(before->instance_owner == after->instance_owner && before->domain_owner == after->domain_owner);
    CHECK(before->budget_owner == after->budget_owner && before->call.work_domain == after->call.work_domain);
    CHECK(before->domain.requested_limit == after->domain.requested_limit);
    CHECK(before->domain.requested_call_limit == after->domain.requested_call_limit);
    CHECK(before->domain.work_limit == after->domain.work_limit && before->domain.call_limit == after->domain.call_limit);
    CHECK(before->domain.metadata_limit == after->domain.metadata_limit);
    CHECK(before->call.byte_limit == after->call.byte_limit && before->call.requested_limit == after->call.requested_limit);
    CHECK(before->call.resume_limit == after->call.resume_limit);
    CHECK(before->value_limit == after->value_limit);
}
static void rr_no_callbacks(const RiWitness *w) {
    const unsigned empty[RI_FUNCTIONS] = {0};
    CHECK(!memcmp(w->calls, empty, sizeof(empty)) && !memcmp(w->releases, empty, sizeof(empty)));
    CHECK(!w->output_count && !w->lifecycle_count && !w->root_call && !w->saved_root_live);
    CHECK(!w->active_view && !w->returned_view && !w->function.type && !w->function.reserved && !w->function.payload);
    if (w->instance) CHECK(!w->instance->publication_count);
}
static XrXirInstanceConfig rr_config(RiWitness *w) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, ri_output, w};
    config.trace = ri_lifecycle; config.trace_context = w;
    return config;
}
static void rr_physical_zero(const char *phase) {
    CHECK(!ri_observed && !runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    effects_source_owners_free();
    CHECK(!effects_compile_live && !effects_compile_bytes);
    printf("RR phase=%s runtime_physical=0/0 compiler_physical=0/0 bookkeeping=0\n", phase);
}
static void rr_program_finish(RiFixture *f) {
    CHECK(f->program && !f->code_releases && f->lowered);
    xr_xir_compile_program_drop(f->program); f->program = NULL;
    CHECK(f->code_releases == 1 && !f->lowered);
    ri_observed = NULL;
    rr_physical_zero("program_release");
}
static void rr_unentered_free(RiWitness *w, XrXirCallStatus expected) {
    rr_no_callbacks(w);
    XrXirDomain *domain = w->instance->domain;
    CHECK(xr_xir_domain_retain(domain));
    RrSnapshot before = rr_snapshot(w);
    XrXirCallStatus status = xr_xir_instance_free(w->instance); w->instance = NULL;
    XrXirDomainBudgetStats released = xr_xir_domain_budget_stats(domain);
    XrXirDomainStats values = xr_xir_domain_stats(domain);
    printf("RR phase=unentered_free expected=%u actual=%u before_work=%llu after_work=%llu "
        "requested_call=%llu metadata_live=%llu call_live=%llu domain_value_live=%llu\n",
        (unsigned)expected, (unsigned)status, (unsigned long long)before.domain.work,
        (unsigned long long)released.work, (unsigned long long)released.requested_call_bytes,
        (unsigned long long)released.metadata_live, (unsigned long long)released.call_live,
        (unsigned long long)values.live_bytes);
    CHECK(status == expected); rr_no_callbacks(w);
    CHECK(released.requested_bytes == before.domain.requested_bytes);
    CHECK(released.requested_call_bytes == before.domain.requested_call_bytes && released.work >= before.domain.work);
    CHECK(!released.metadata_live && !released.call_live);
    CHECK(released.metadata_allocations == released.metadata_frees && released.call_allocations == released.call_frees);
    CHECK(values.live_bytes == sizeof(XrXirDomain) && values.allocations == values.frees + 1);
    xr_xir_domain_drop(domain);
}
static void rr_complete_pair(RiFixture *f, bool first_prepared) {
    CHECK(runtime_fail_at == SIZE_MAX && effects_compile_fail_at == SIZE_MAX);
    XrXirInstanceResult suspended[RI_INSTANCES];
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        RiWitness *w = &f->witnesses[i];
        if (i || !first_prepared) ri_start_root(w);
        suspended[i] = ri_suspended(w);
    }
    ri_cross_gate(f);
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        RiWitness *w = &f->witnesses[i];
        CHECK(xr_xir_instance_resume(w->instance, suspended[i].epoch, suspended[i].outcome.wake) == XR_XIR_CALL_READY);
        ri_returned(w); CHECK(ri_counter(w) == 11); ri_output_trace(w, 1);
        CHECK(w->root_cleanup_slot == 11 && w->child_local == 1);
        CHECK(w->releases[RI_ROOT] == 1 && w->releases[RI_CHILD] == 1 && w->releases[RI_READ] == 1);
        CHECK(w->releases[RI_ROOT_EXIT] == 1 && w->releases[RI_CHILD_EXIT] == 1);
        RrSnapshot complete = rr_snapshot(w); rr_log_snapshot("vm_completed_42_output6_42_5_counter11", i,
            XR_XIR_CALL_RETURNED, &complete);
    }
    ri_finish(f, false); rr_physical_zero("vm_pair_release");
}
static size_t rr_create_census(void) {
    RiFixture f; ri_fixture(&f, false);
    RiWitness *w = &f.witnesses[0]; XrXirInstanceConfig config = rr_config(w);
    size_t begin = runtime_attempts;
    CHECK(xr_xir_instance_new(f.program, &config, &w->instance) == XR_XIR_CALL_READY);
    size_t sites = runtime_attempts - begin; CHECK(sites && sites < 128);
    RrSnapshot created = rr_snapshot(w); rr_log_snapshot("create_census", sites, XR_XIR_CALL_READY, &created);
    CHECK(created.state == XR_XIR_INSTANCE_NEW && !created.epoch && !created.prepared);
    rr_unentered_free(w, XR_XIR_CALL_READY); rr_program_finish(&f);
    return sites;
}
static RrCensus rr_prepare_census(void) {
    RiFixture f; ri_fixture(&f, false); ri_instances(&f);
    RiWitness *w = &f.witnesses[0]; RrSnapshot before = rr_snapshot(w);
    rr_log_snapshot("prepare_census_before", 0, XR_XIR_CALL_READY, &before);
    CHECK(xr_xir_instance_start(w->instance, RI_ROOT, NULL, 0) == XR_XIR_CALL_READY);
    RrSnapshot prepared = rr_snapshot(w); size_t sites = prepared.attempts - before.attempts;
    rr_log_snapshot("prepare_census_after", sites, XR_XIR_CALL_READY, &prepared);
    CHECK(sites == 2 && prepared.epoch == 1 && prepared.state == XR_XIR_INSTANCE_INITIALIZING);
    CHECK(w->instance->call && w->instance->call->segment && !w->instance->call->segment->parent);
    const XrXirCall *call = w->instance->call;
    RrCensus census = {0, sites, before.domain.requested_call_bytes, prepared.domain.requested_call_bytes,
        before.domain.work, prepared.domain.work, prepared.call.requested_bytes - before.call.requested_bytes,
        prepared.domain.work - before.domain.work, call->allocation_bytes, call->segment->allocation_bytes,
        call->allocation_bytes + call->segment->used};
    CHECK(census.prepare_requested == census.call_bytes + census.segment_bytes);
    CHECK(census.prepare_requested == prepared.domain.requested_call_bytes - before.domain.requested_call_bytes);
    CHECK(census.prepare_work == sites && census.minimum_call_live < prepared.call.peak_bytes);
    printf("RR census prepare=%zu call_bytes=%llu segment_bytes=%llu occupied_call_live=%llu "
        "default_call_peak=%llu requested_exact=%llu work_exact=%llu\n", sites,
        (unsigned long long)census.call_bytes, (unsigned long long)census.segment_bytes,
        (unsigned long long)census.minimum_call_live, (unsigned long long)prepared.call.peak_bytes,
        (unsigned long long)census.prepared_requested, (unsigned long long)census.prepared_work);
    rr_complete_pair(&f, true);
    return census;
}
static void rr_create_oom(void) {
    const size_t sites = rr_create_census();
    for (size_t point = 0; point < sites; ++point) {
        RiFixture f; ri_fixture(&f, false);
        RiWitness *w = &f.witnesses[0]; XrXirInstanceConfig config = rr_config(w);
        RrPhysical baseline = rr_physical();
        uint32_t references = atomic_load_explicit(&f.program->references, memory_order_relaxed);
        size_t begin = runtime_attempts; CHECK(point < SIZE_MAX - begin);
        runtime_fail_at = begin + point;
        XrXirCallStatus status = xr_xir_instance_new(f.program, &config, &w->instance);
        const bool injected = runtime_attempts == begin + point + 1;
        runtime_fail_at = SIZE_MAX;
        printf("RR phase=create_oom point=%zu sites=%zu actual=%u injected=%u attempts=%zu "
            "retry_owner=Program runtime_ledger_published=0\n", point, sites, (unsigned)status,
            (unsigned)injected, runtime_attempts - begin);
        CHECK(status == XR_XIR_CALL_OOM && injected && !w->instance);
        rr_no_callbacks(w); rr_same_physical(baseline); CHECK(!f.code_releases);
        CHECK(atomic_load_explicit(&f.program->references, memory_order_relaxed) == references);
        ri_instances(&f);
        rr_complete_pair(&f, false);
    }
    printf("RR create_oom actual_sites=%zu all_injected=1 same_Program_retry=1 full_VM_oracle=1 physical0=1\n", sites);
}
static void rr_failed_prepare(const RrCensus *c, size_t point, const RrSnapshot *before,
    const RrSnapshot *failed) {
    rr_same_owner(before, failed);
    CHECK(failed->state == XR_XIR_INSTANCE_NEW && !failed->epoch && !failed->prepared && !failed->call.exhausted);
    CHECK(failed->attempts == before->attempts + point + 1);
    uint64_t charged = point ? c->prepare_requested : c->call_bytes;
    CHECK(failed->domain.requested_call_bytes == before->domain.requested_call_bytes + charged);
    CHECK(failed->call.requested_bytes == before->call.requested_bytes + charged);
    CHECK(failed->domain.work == before->domain.work + point + 1);
    CHECK(failed->domain.requested_bytes == before->domain.requested_bytes);
    CHECK(failed->values.live_bytes == before->values.live_bytes && failed->values.allocations == before->values.allocations);
    CHECK(failed->values.frees == before->values.frees);
    CHECK(failed->domain.metadata_live == before->domain.metadata_live);
    CHECK(failed->domain.metadata_allocations == before->domain.metadata_allocations);
    CHECK(failed->domain.metadata_frees == before->domain.metadata_frees);
    CHECK(failed->call.live_bytes == before->call.live_bytes && failed->domain.call_live == before->domain.call_live);
    CHECK(failed->call.allocations == before->call.allocations + point && failed->call.frees == before->call.frees + point);
    CHECK(failed->domain.call_allocations == before->domain.call_allocations + point);
    CHECK(failed->domain.call_frees == before->domain.call_frees + point);
}
static void rr_prepare_oom(void) {
    const RrCensus census = rr_prepare_census();
    for (size_t point = 0; point < census.prepare_sites; ++point) {
        RiFixture f; ri_fixture(&f, false); ri_instances(&f);
        RiWitness *w = &f.witnesses[0]; RrSnapshot before = rr_snapshot(w);
        XrXirInstanceConfig unchanged; memcpy(&unchanged, &w->instance->config, sizeof(unchanged));
        RrPhysical baseline = rr_physical();
        rr_log_snapshot("prepare_oom_before", point, XR_XIR_CALL_READY, &before);
        CHECK(point < SIZE_MAX - runtime_attempts); runtime_fail_at = runtime_attempts + point;
        XrXirCallStatus status = xr_xir_instance_start(w->instance, RI_ROOT, NULL, 0);
        const bool injected = runtime_attempts == before.attempts + point + 1;
        runtime_fail_at = SIZE_MAX; RrSnapshot failed = rr_snapshot(w);
        rr_log_snapshot("prepare_oom_failed", point, status, &failed);
        printf("RR phase=prepare_oom_injection point=%zu sites=%zu injected=%u\n",
            point, census.prepare_sites, (unsigned)injected);
        CHECK(status == XR_XIR_CALL_OOM && injected); rr_failed_prepare(&census, point, &before, &failed);
        CHECK(!memcmp(&unchanged, &w->instance->config, sizeof(unchanged)));
        rr_no_callbacks(w); rr_no_callbacks(&f.witnesses[1]); rr_same_physical(baseline);
        status = xr_xir_instance_start(w->instance, RI_ROOT, NULL, 0);
        RrSnapshot retry = rr_snapshot(w); rr_log_snapshot("prepare_oom_same_owner_retry", point, status, &retry);
        CHECK(status == XR_XIR_CALL_READY); rr_same_owner(&before, &retry);
        CHECK(!memcmp(&unchanged, &w->instance->config, sizeof(unchanged)));
        CHECK(retry.state == XR_XIR_INSTANCE_INITIALIZING && retry.epoch == 1 && retry.prepared);
        CHECK(retry.domain.requested_call_bytes == failed.domain.requested_call_bytes + census.prepare_requested);
        CHECK(retry.call.requested_bytes == failed.call.requested_bytes + census.prepare_requested);
        CHECK(retry.domain.work == failed.domain.work + census.prepare_work);
        CHECK(retry.attempts == failed.attempts + census.prepare_sites && !retry.call.exhausted);
        CHECK(retry.call.live_bytes == census.prepare_requested && retry.domain.call_live == census.prepare_requested);
        rr_complete_pair(&f, true);
    }
    printf("RR prepare_oom actual_sites=%zu all_injected=1 same_Instance_Domain_CallBudget=1 "
        "no_refund=1 full_VM_oracle=1 physical0=1\n", census.prepare_sites);
}
enum { RR_REQUESTED, RR_CALL_LIVE, RR_WORK, RR_AXES };
static void rr_prepare_axis(const RrCensus *c, unsigned axis, unsigned minus) {
    RiFixture f; ri_fixture(&f, false); RiWitness *w = &f.witnesses[0];
    XrXirInstanceConfig config = rr_config(w);
    const char *name = axis == RR_REQUESTED ? "requested_call" : axis == RR_CALL_LIVE ? "call_live" : "work";
    uint64_t boundary = axis == RR_REQUESTED ? c->prepared_requested :
        axis == RR_CALL_LIVE ? c->minimum_call_live : c->prepared_work;
    CHECK(boundary > 1 && axis < RR_AXES && minus < 2);
    if (axis == RR_REQUESTED) config.requested_call_limit = boundary - minus;
    else if (axis == RR_CALL_LIVE) config.call_limit = boundary - minus;
    else config.work_limit = boundary - minus;
    CHECK(xr_xir_instance_new(f.program, &config, &w->instance) == XR_XIR_CALL_READY);
    RrSnapshot before = rr_snapshot(w); rr_log_snapshot("budget_before", axis * 2 + minus, XR_XIR_CALL_READY, &before);
    XrXirCallStatus status = xr_xir_instance_start(w->instance, RI_ROOT, NULL, 0);
    RrSnapshot after = rr_snapshot(w); rr_log_snapshot("budget_after", axis * 2 + minus, status, &after);
    printf("RR phase=prepare_budget axis=%s minus=%u boundary=%llu limit=%llu actual=%u attempts=%zu\n",
        name, minus, (unsigned long long)boundary, (unsigned long long)(boundary - minus),
        (unsigned)status, after.attempts - before.attempts);
    CHECK(status == (minus ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY)); rr_same_owner(&before, &after);
    CHECK(after.state == (minus ? XR_XIR_INSTANCE_NEW : XR_XIR_INSTANCE_INITIALIZING));
    CHECK(after.epoch == (minus ? 0u : 1u) && after.prepared == !minus);
    CHECK(after.attempts - before.attempts == (minus ? 1u : c->prepare_sites));
    CHECK(after.call.exhausted == (minus && axis != RR_CALL_LIVE));
    if (!minus && axis == RR_REQUESTED) CHECK(after.domain.requested_call_bytes == boundary);
    if (!minus && axis == RR_CALL_LIVE) CHECK(after.call.live_bytes == boundary && after.domain.call_live == boundary);
    if (!minus && axis == RR_WORK) CHECK(after.domain.work == boundary);
    rr_no_callbacks(w);
    XrXirCallStatus expected_free = axis == RR_WORK || (minus && axis == RR_REQUESTED) ?
        XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY;
    rr_unentered_free(w, expected_free); rr_program_finish(&f);
}
static void rr_oom_same_quota(const RrCensus *c) {
    RiFixture f; ri_fixture(&f, false); RiWitness *w = &f.witnesses[0];
    XrXirInstanceConfig config = rr_config(w); config.requested_call_limit = c->prepared_requested;
    CHECK(xr_xir_instance_new(f.program, &config, &w->instance) == XR_XIR_CALL_READY);
    RrSnapshot before = rr_snapshot(w);
    rr_log_snapshot("same_quota_before", 0, XR_XIR_CALL_READY, &before);
    CHECK(c->prepare_sites - 1 < SIZE_MAX - runtime_attempts);
    runtime_fail_at = runtime_attempts + c->prepare_sites - 1;
    XrXirCallStatus status = xr_xir_instance_start(w->instance, RI_ROOT, NULL, 0);
    const bool injected = runtime_attempts == before.attempts + c->prepare_sites;
    runtime_fail_at = SIZE_MAX; RrSnapshot failed = rr_snapshot(w);
    rr_log_snapshot("same_quota_oom", 0, status, &failed);
    CHECK(status == XR_XIR_CALL_OOM && injected);
    rr_failed_prepare(c, c->prepare_sites - 1, &before, &failed);
    CHECK(failed.domain.requested_call_bytes == config.requested_call_limit);
    status = xr_xir_instance_start(w->instance, RI_ROOT, NULL, 0);
    RrSnapshot retry = rr_snapshot(w); rr_log_snapshot("same_quota_retry_limit", 0, status, &retry);
    CHECK(status == XR_XIR_CALL_LIMIT); rr_same_owner(&before, &retry);
    CHECK(retry.attempts == failed.attempts && retry.domain.requested_call_bytes == failed.domain.requested_call_bytes);
    CHECK(retry.domain.work == failed.domain.work && retry.call.requested_bytes == failed.call.requested_bytes);
    CHECK(retry.state == XR_XIR_INSTANCE_NEW && !retry.epoch && !retry.prepared && retry.call.exhausted);
    rr_no_callbacks(w); rr_unentered_free(w, XR_XIR_CALL_LIMIT); rr_program_finish(&f);
    printf("RR same_quota actual_oom_points=1 injected=1 retry=LIMIT allocations=0 no_refund=1 no_topup=1 physical0=1\n");
}
static void rr_prepare_budget(void) {
    const RrCensus census = rr_prepare_census();
    for (unsigned axis = 0; axis < RR_AXES; ++axis)
        for (unsigned minus = 0; minus < 2; ++minus) rr_prepare_axis(&census, axis, minus);
    rr_oom_same_quota(&census);
    printf("RR prepare_budget axes=3 exact_minus1_controls=6 extra_same_quota_oom=1 "
        "frontier=before_first_poll whole_program_budget=OPEN\n");
}
static void rr_normal(void) {
    ri_identity_case(); rr_physical_zero("normal_identity");
    ri_resume_case(); rr_physical_zero("normal_resume");
    ri_closing_case(); rr_physical_zero("normal_closing");
    ri_initialization_failure_case(); rr_physical_zero("normal_initialization_failure");
    size_t sites = rr_create_census(); RrCensus census = rr_prepare_census(); census.create_sites = sites;
    printf("RR normal create_sites=%zu prepare_sites=%zu original_four_oracles=PASS "
        "resource_all_site_qualification=OPEN full_P3=OPEN task322=OPEN goal=OPEN\n",
        census.create_sites, census.prepare_sites);
}
#endif // XIR_ROOT_IDENTITY_VM_RESOURCES_PREPARE_H
