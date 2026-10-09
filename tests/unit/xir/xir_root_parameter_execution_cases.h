/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_parameter_execution_cases.h - Independent typed output and instance witnesses
 *
 * KEY CONCEPT:
 *   Observation reads actual owners without manufacturing execution authority.
 */
#ifndef XIR_ROOT_PARAMETER_EXECUTION_CASES_H
#define XIR_ROOT_PARAMETER_EXECUTION_CASES_H
#include "xir_root_parameter_oracles.h"

typedef struct H1ExecObservation {
    const RootParameterSourceOracle *oracle;
    XrXirOutputSink sink;
    uint32_t module, slots, groups, writes, begins, ready, published, released;
    char bytes[8];
    size_t length;
} H1ExecObservation;

typedef struct H1ExecLedger {
    XrXirDomainBudgetStats domain;
    XrXirDomainStats values;
    XrXirCallBudget call;
} H1ExecLedger;

typedef struct H1ExecFixture {
    XrXirProgram *program;
    uint32_t entry, run, module, slots;
} H1ExecFixture;

static void h1exec_value(const XrXirValue *value, int64_t expected) {
    CHECK(value && value->type == XR_XIR_I64 && !value->reserved && value->payload == expected);
    CHECK(xr_xir_value_valid(value) && xr_xir_value_arena(value) == NULL);
}

static XrXirOutputStatus h1exec_bytes(void *opaque, XrXirOutputStream stream,
                                   const char *bytes, size_t length) {
    H1ExecObservation *o = opaque;
    CHECK(stream == XR_XIR_STDOUT && !o->writes && length == o->oracle->output_bytes);
    CHECK(length <= sizeof(o->bytes) && !memcmp(bytes, o->oracle->output, length));
    memcpy(o->bytes, bytes, length); o->length = length; ++o->writes;
    return XR_XIR_OUTPUT_OK;
}

static XrXirOutputStatus h1exec_typed(void *opaque, const XrXirOutputGroup *group) {
    H1ExecObservation *o = opaque;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1);
    CHECK(group->values && !o->groups);
    h1exec_value(&group->values[0], o->oracle->result);
    ++o->groups;
    return xr_xir_output_render(&o->sink, group);
}

static void h1exec_lifecycle(void *opaque, XrXirLifecycleEvent event, uint32_t index) {
    H1ExecObservation *o = opaque;
    if (event == XR_XIR_MODULE_BEGIN) {
        CHECK(index == o->module && !o->begins && !o->ready); ++o->begins;
    } else if (event == XR_XIR_MODULE_READY) {
        CHECK(index == o->module && o->begins == 1 && !o->ready); ++o->ready;
    } else {
        CHECK(o->slots == 1 && index == 0);
        if (event == XR_XIR_SLOT_PUBLISHED) {
            CHECK(o->begins == 1 && !o->published && !o->released); ++o->published;
        } else {
            CHECK(event == XR_XIR_SLOT_RELEASED && o->published == 1 && !o->released); ++o->released;
        }
    }
}

static H1ExecLedger h1exec_ledger(const XrXirInstance *instance) {
    CHECK(instance && instance->budget.work_domain == instance->domain);
    return (H1ExecLedger){xr_xir_domain_budget_stats(instance->domain),
        xr_xir_domain_stats(instance->domain), instance->budget};
}

static void h1exec_unchanged(const XrXirInstance *instance, const H1ExecLedger *before) {
    H1ExecLedger after = h1exec_ledger(instance);
#define H1VM_EQ(field) CHECK(after.domain.field == before->domain.field)
    H1VM_EQ(requested_bytes); H1VM_EQ(requested_limit); H1VM_EQ(work); H1VM_EQ(work_limit); H1VM_EQ(bound);
    H1VM_EQ(requested_call_bytes); H1VM_EQ(requested_call_limit);
    H1VM_EQ(metadata_live); H1VM_EQ(metadata_peak); H1VM_EQ(metadata_limit);
    H1VM_EQ(metadata_allocations); H1VM_EQ(metadata_frees);
    H1VM_EQ(call_live); H1VM_EQ(call_peak); H1VM_EQ(call_limit); H1VM_EQ(call_allocations); H1VM_EQ(call_frees);
#undef H1VM_EQ
    CHECK(after.values.live_bytes == before->values.live_bytes && after.values.peak_bytes == before->values.peak_bytes);
    CHECK(after.values.allocations == before->values.allocations && after.values.frees == before->values.frees &&
        after.values.reallocations == before->values.reallocations);
#define H1VM_CALL_EQ(field) CHECK(after.call.field == before->call.field)
    H1VM_CALL_EQ(byte_limit); H1VM_CALL_EQ(requested_limit); H1VM_CALL_EQ(requested_bytes);
    H1VM_CALL_EQ(live_bytes); H1VM_CALL_EQ(peak_bytes); H1VM_CALL_EQ(allocations); H1VM_CALL_EQ(frees);
    H1VM_CALL_EQ(resume_limit); H1VM_CALL_EQ(resumes); H1VM_CALL_EQ(transitions);
    H1VM_CALL_EQ(release_tickets); H1VM_CALL_EQ(released_frames); H1VM_CALL_EQ(work_domain); H1VM_CALL_EQ(exhausted);
#undef H1VM_CALL_EQ
}

static void h1exec_runtime_report(const RootParameterSourceOracle *oracle, const char *phase,
    unsigned i, const XrXirInstance *instance, size_t sites) {
    H1ExecLedger s = h1exec_ledger(instance);
    CHECK(instance->config.metadata_limit == 65536 && instance->config.value_limit == 65536 &&
        instance->config.call_limit == 65536 && instance->config.poll_limit == 8000 && instance->config.depth_limit == 96);
    CHECK(instance->config.requested_value_limit == UINT64_C(67108864) &&
        instance->config.requested_call_limit == UINT64_C(67108864) && instance->config.work_limit == UINT64_C(128000000));
    CHECK(s.call.resumes <= 8000 && !s.call.exhausted);
    printf("H1_EXEC_RUNTIME file=%s phase=%s instance=%u owner=%p domain=%p gate=%p budget=%p program=%p "
        "sites=%zu resumes=%llu transitions=%llu value_requested=%llu call_requested=%llu work=%llu "
        "value_live=%llu value_peak=%llu metadata_live=%llu metadata_peak=%llu call_live=%llu call_peak=%llu\n",
        oracle->file, phase, i, (const void *)instance, (void *)instance->domain,
        (void *)instance->function_gate, (const void *)&instance->budget, (void *)instance->program,
        sites, (unsigned long long)s.call.resumes, (unsigned long long)s.call.transitions,
        (unsigned long long)s.domain.requested_bytes, (unsigned long long)s.domain.requested_call_bytes,
        (unsigned long long)s.domain.work, (unsigned long long)s.values.live_bytes,
        (unsigned long long)s.values.peak_bytes, (unsigned long long)s.domain.metadata_live,
        (unsigned long long)s.domain.metadata_peak, (unsigned long long)s.domain.call_live,
        (unsigned long long)s.domain.call_peak);
}

static void h1exec_output_done(const H1ExecObservation *o) {
    CHECK(o->begins == 1 && o->ready == 1 && o->groups == 1 && o->writes == 1);
    CHECK(o->length == o->oracle->output_bytes && !memcmp(o->bytes, o->oracle->output, o->length));
    CHECK(o->published == o->slots);
}

static void h1exec_output_unchanged(const H1ExecObservation *now, const H1ExecObservation *before) {
    CHECK(now->groups == before->groups && now->writes == before->writes && now->length == before->length);
    CHECK(now->begins == before->begins && now->ready == before->ready && now->published == before->published &&
        now->released == before->released && !memcmp(now->bytes, before->bytes, sizeof(now->bytes)));
}

static void h1exec_drive_pair(XrXirInstance *instances[2], H1ExecObservation observations[2],
                          XrXirValue values[2], bool initialization) {
    bool done[2] = {false, false}; unsigned polls[2] = {0, 0};
    size_t sites[2] = {0, 0};
    while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) {
        if (done[i]) continue;
        H1ExecLedger peer = h1exec_ledger(instances[1 - i]);
        H1ExecObservation peer_output = observations[1 - i];
        size_t begin = runtime_attempts;
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(instances[i], 1);
        CHECK(++polls[i] <= 16000);
        h1exec_unchanged(instances[1 - i], &peer);
        h1exec_output_unchanged(&observations[1 - i], &peer_output);
        if (result.outcome.status == XR_XIR_CALL_READY) { sites[i] += runtime_attempts - begin; continue; }
        if (result.outcome.status != XR_XIR_CALL_RETURNED)
            fprintf(stderr, "H1 consumer file=%s initialization=%u instance=%u poll=%u status=%u state=%u\n",
                observations[i].oracle->file, (unsigned)initialization, i, polls[i],
                (unsigned)result.outcome.status, (unsigned)xr_xir_instance_state(instances[i]));
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && xr_xir_call_result_valid(&result.outcome));
        CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_READY && result.epoch);
        h1exec_value(&result.outcome.value, initialization ? 0 : observations[i].oracle->result);
        XrXirValue occupied = {XR_XIR_I64, 0, INT64_C(54321)}, saved = occupied;
        CHECK(xr_xir_instance_take_result(instances[i], &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!memcmp(&occupied, &saved, sizeof(occupied)));
        CHECK(xr_xir_instance_take_result(instances[i], &values[i]) == XR_XIR_CALL_RETURNED);
        h1exec_value(&values[i], initialization ? 0 : observations[i].oracle->result);
        XrXirValue empty = {0};
        CHECK(xr_xir_instance_take_result(instances[i], &empty) == XR_XIR_CALL_BAD_STATE);
        CHECK(!empty.type && !empty.reserved && !empty.payload);
        h1exec_output_done(&observations[i]); h1exec_unchanged(instances[1 - i], &peer);
        h1exec_output_unchanged(&observations[1 - i], &peer_output);
        sites[i] += runtime_attempts - begin;
        done[i] = true;
        h1exec_runtime_report(observations[i].oracle, initialization ? "entry_returned" : "run_returned",
            i, instances[i], sites[i]);
    }
}

static void h1exec_domain_only(XrXirDomain *domain) {
    XrXirDomainBudgetStats d = xr_xir_domain_budget_stats(domain);
    XrXirDomainStats v = xr_xir_domain_stats(domain);
    CHECK(!d.metadata_live && !d.call_live && d.metadata_allocations == d.metadata_frees &&
        d.call_allocations == d.call_frees);
    CHECK(v.live_bytes == sizeof(*domain) && v.allocations == v.frees + 1);
}
static void h1exec_pair(const RootParameterSourceOracle *oracle, H1ExecFixture fixture) {
    H1ExecObservation o[2] = {{0}, {0}}; XrXirInstance *instances[2] = {NULL, NULL};
    XrXirDomain *domains[2] = {NULL, NULL};
    for (unsigned i = 0; i < 2; ++i) {
        o[i].oracle = oracle; o[i].module = fixture.module; o[i].slots = fixture.slots;
        o[i].sink = (XrXirOutputSink){XR_XIR_CALL_ABI_VERSION, 0, h1exec_bytes, &o[i], 65536};
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.metadata_limit = 65536; config.value_limit = 65536; config.call_limit = 65536;
        config.poll_limit = 8000; config.depth_limit = 96;
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, h1exec_typed, &o[i]};
        config.trace = h1exec_lifecycle; config.trace_context = &o[i];
        size_t begin = runtime_attempts;
        CHECK(xr_xir_instance_new(fixture.program, &config, &instances[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_NEW);
        domains[i] = instances[i]->domain; CHECK(xr_xir_domain_retain(domains[i]));
        h1exec_runtime_report(oracle, "created", i, instances[i], runtime_attempts - begin);
    }
    CHECK(instances[0] != instances[1] && domains[0] != domains[1] &&
        &instances[0]->budget != &instances[1]->budget && instances[0]->program == instances[1]->program);
    xr_xir_compile_program_drop(fixture.program); fixture.program = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        H1ExecLedger peer = h1exec_ledger(instances[1 - i]);
        H1ExecObservation peer_output = o[1 - i]; size_t begin = runtime_attempts;
        CHECK(xr_xir_instance_start(instances[i], fixture.entry, NULL, 0) == XR_XIR_CALL_READY);
        h1exec_unchanged(instances[1 - i], &peer); h1exec_output_unchanged(&o[1 - i], &peer_output);
        h1exec_runtime_report(oracle, "entry_start", i, instances[i], runtime_attempts - begin);
    }
    XrXirValue entry[2] = {{0}, {0}}, held[2] = {{0}, {0}}, aliases[2] = {{0}, {0}};
    h1exec_drive_pair(instances, o, entry, true);
    for (unsigned i = 0; i < 2; ++i) {
        xr_xir_value_drop(&entry[i]);
        CHECK(!entry[i].type && !entry[i].reserved && !entry[i].payload);
        CHECK(instances[i]->function_gate && o[i].published == fixture.slots);
        if (fixture.slots) h1exec_value(&instances[i]->slots[0], 9);
        H1ExecLedger peer = h1exec_ledger(instances[1 - i]);
        H1ExecObservation peer_output = o[1 - i]; size_t begin = runtime_attempts;
        CHECK(xr_xir_instance_start(instances[i], fixture.run, NULL, 0) == XR_XIR_CALL_READY);
        h1exec_unchanged(instances[1 - i], &peer); h1exec_output_unchanged(&o[1 - i], &peer_output);
        h1exec_runtime_report(oracle, "run_start", i, instances[i], runtime_attempts - begin);
    }
    CHECK(instances[0]->function_gate != instances[1]->function_gate);
    h1exec_drive_pair(instances, o, held, false);
    for (unsigned i = 0; i < 2; ++i) {
        H1ExecLedger peer = h1exec_ledger(instances[1 - i]);
        CHECK(xr_xir_value_copy(&held[i], &aliases[i]) == XR_XIR_VALUE_OK);
        h1exec_value(&aliases[i], oracle->result); h1exec_unchanged(instances[1 - i], &peer);
    }
    H1ExecLedger peer = h1exec_ledger(instances[1]); H1ExecObservation peer_output = o[1];
    CHECK(xr_xir_instance_stop(instances[0]) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(instances[0]) == XR_XIR_CALL_READY); instances[0] = NULL;
    h1exec_domain_only(domains[0]); h1exec_unchanged(instances[1], &peer); h1exec_output_unchanged(&o[1], &peer_output);
    CHECK(o[0].released == fixture.slots && instance_compile_live && instance_compile_bytes);
    h1exec_value(&held[0], oracle->result);
    CHECK(xr_xir_instance_stop(instances[1]) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(instances[1]) == XR_XIR_CALL_READY); instances[1] = NULL;
    h1exec_domain_only(domains[1]); CHECK(o[1].released == fixture.slots); instance_compile_zero();
    size_t begin = runtime_attempts;
    for (unsigned i = 0; i < 2; ++i) {
        h1exec_output_done(&o[i]); h1exec_value(&held[i], oracle->result); h1exec_value(&aliases[i], oracle->result);
        xr_xir_value_drop(&held[i]); xr_xir_value_drop(&aliases[i]);
        CHECK(!held[i].type && !held[i].reserved && !held[i].payload &&
            !aliases[i].type && !aliases[i].reserved && !aliases[i].payload);
        printf("H1_EXEC_OUTPUT file=%s instance=%u typed_groups=%u typed_type=I64 value=%lld writes=%u bytes=",
            oracle->file, i, o[i].groups, (long long)oracle->result, o[i].writes);
        for (size_t b = 0; b < o[i].length; ++b) printf("%02x", (unsigned)(uint8_t)o[i].bytes[b]);
        printf(" begins=%u ready=%u published=%u released=%u\n", o[i].begins, o[i].ready, o[i].published, o[i].released);
        xr_xir_domain_drop(domains[i]); domains[i] = NULL;
    }
    CHECK(runtime_attempts == begin && !runtime_live && !runtime_bytes); instance_compile_zero();
    printf("H1_EXEC_PHYSICAL file=%s instances=2 compiler_blocks=0 compiler_bytes=0 runtime_blocks=0 runtime_bytes=0\n", oracle->file);
}

#endif // XIR_ROOT_PARAMETER_EXECUTION_CASES_H
