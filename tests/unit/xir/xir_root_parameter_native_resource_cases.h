/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_parameter_native_resource_cases.h - Independent typed output and instance witnesses
 *
 * KEY CONCEPT:
 *   Observation reads actual owners without manufacturing execution authority.
 */
#ifndef XIR_ROOT_PARAMETER_NATIVE_RESOURCE_CASES_H
#define XIR_ROOT_PARAMETER_NATIVE_RESOURCE_CASES_H
#include "xir_root_parameter_oracles.h"

typedef struct H1NrObservation {
    const RootParameterSourceOracle *oracle;
    XrXirInstance *instance;
    XrXirOutputSink sink;
    uint32_t module, slots, groups, writes, begins, ready, published, released;
    XrXirValue observed; /* I64 snapshot taken only after the actual typed value passes checks. */
    char bytes[8];
    size_t length;
    XrXirOutputStatus byte_failure;
    size_t renderer_begin, renderer_end;
    unsigned renderer_oom;
} H1NrObservation;

typedef struct H1NrLedger {
    XrXirDomainBudgetStats domain;
    XrXirDomainStats values;
    XrXirCallBudget call;
} H1NrLedger;

typedef struct H1NrFixture {
    XrXirProgram *program;
    uint32_t entry, run, module, slots;
} H1NrFixture;

static H1NrLedger h1nr_ledger(const XrXirInstance *instance);
static void h1nr_unchanged(const XrXirInstance *instance, const H1NrLedger *before);

static void h1nr_value(const XrXirValue *value, int64_t expected) {
    CHECK(value && value->type == XR_XIR_I64 && !value->reserved && value->payload == expected);
    CHECK(xr_xir_value_valid(value) && xr_xir_value_arena(value) == NULL);
}

static XrXirOutputStatus h1nr_bytes(void *opaque, XrXirOutputStream stream,
                                   const char *bytes, size_t length) {
    H1NrObservation *o = opaque;
    CHECK(stream == XR_XIR_STDOUT && !o->writes && length == o->oracle->output_bytes);
    CHECK(length <= sizeof(o->bytes) && !memcmp(bytes, o->oracle->output, length));
    if (o->byte_failure != XR_XIR_OUTPUT_OK) return o->byte_failure;
    memcpy(o->bytes, bytes, length); o->length = length; ++o->writes;
    return XR_XIR_OUTPUT_OK;
}

static XrXirOutputStatus h1nr_typed(void *opaque, const XrXirOutputGroup *group) {
    H1NrObservation *o = opaque;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 1);
    CHECK(group->values && !o->groups);
    h1nr_value(&group->values[0], o->oracle->result);
    o->observed = group->values[0];
    ++o->groups;
    CHECK(o->instance); H1NrLedger fees = h1nr_ledger(o->instance);
    o->renderer_begin = runtime_attempts;
    XrXirOutputStatus status = xr_xir_output_render(&o->sink, group);
    o->renderer_end = runtime_attempts; CHECK(o->renderer_end == o->renderer_begin + 1);
    h1nr_unchanged(o->instance, &fees);
    if (status == XR_XIR_OUTPUT_OOM) { CHECK(!o->writes && !o->length); ++o->renderer_oom; }
    return status;
}

static void h1nr_lifecycle(void *opaque, XrXirLifecycleEvent event, uint32_t index) {
    H1NrObservation *o = opaque;
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

static H1NrLedger h1nr_ledger(const XrXirInstance *instance) {
    CHECK(instance && instance->budget.work_domain == instance->domain);
    return (H1NrLedger){xr_xir_domain_budget_stats(instance->domain),
        xr_xir_domain_stats(instance->domain), instance->budget};
}

static void h1nr_unchanged(const XrXirInstance *instance, const H1NrLedger *before) {
    H1NrLedger after = h1nr_ledger(instance);
#define H1NR_EQ(field) CHECK(after.domain.field == before->domain.field)
    H1NR_EQ(requested_bytes); H1NR_EQ(requested_limit); H1NR_EQ(work); H1NR_EQ(work_limit); H1NR_EQ(bound);
    H1NR_EQ(requested_call_bytes); H1NR_EQ(requested_call_limit);
    H1NR_EQ(metadata_live); H1NR_EQ(metadata_peak); H1NR_EQ(metadata_limit);
    H1NR_EQ(metadata_allocations); H1NR_EQ(metadata_frees);
    H1NR_EQ(call_live); H1NR_EQ(call_peak); H1NR_EQ(call_limit); H1NR_EQ(call_allocations); H1NR_EQ(call_frees);
#undef H1NR_EQ
    CHECK(after.values.live_bytes == before->values.live_bytes && after.values.peak_bytes == before->values.peak_bytes);
    CHECK(after.values.allocations == before->values.allocations && after.values.frees == before->values.frees &&
        after.values.reallocations == before->values.reallocations);
#define H1NR_CALL_EQ(field) CHECK(after.call.field == before->call.field)
    H1NR_CALL_EQ(byte_limit); H1NR_CALL_EQ(requested_limit); H1NR_CALL_EQ(requested_bytes);
    H1NR_CALL_EQ(live_bytes); H1NR_CALL_EQ(peak_bytes); H1NR_CALL_EQ(allocations); H1NR_CALL_EQ(frees);
    H1NR_CALL_EQ(resume_limit); H1NR_CALL_EQ(resumes); H1NR_CALL_EQ(transitions);
    H1NR_CALL_EQ(release_tickets); H1NR_CALL_EQ(released_frames); H1NR_CALL_EQ(work_domain); H1NR_CALL_EQ(exhausted);
#undef H1NR_CALL_EQ
}

static void h1nr_runtime_report(const RootParameterSourceOracle *oracle, const char *phase,
    unsigned i, const XrXirInstance *instance, size_t sites) {
    H1NrLedger s = h1nr_ledger(instance);
    CHECK(instance->config.metadata_limit == 65536 && instance->config.value_limit == 65536 &&
        instance->config.call_limit == 65536 && instance->config.poll_limit == 8000 && instance->config.depth_limit == 96);
    CHECK(instance->config.requested_value_limit == UINT64_C(67108864) &&
        instance->config.requested_call_limit == UINT64_C(67108864) && instance->config.work_limit == UINT64_C(128000000));
    CHECK(s.call.resumes <= 8000 && !s.call.exhausted);
    printf("H1_NATIVE_RESOURCE_RUNTIME file=%s phase=%s instance=%u owner=%p domain=%p gate=%p budget=%p program=%p "
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

static void h1nr_output_report(const H1NrObservation *o, unsigned instance, uintptr_t owner) {
    printf("H1_NATIVE_RESOURCE_OUTPUT file=%s instance=%u owner=0x%" PRIxPTR " typed_groups=%u typed_type=%u value=%lld writes=%u bytes=",
        o->oracle->file, instance, owner, o->groups, o->observed.type, (long long)o->observed.payload, o->writes);
    for (size_t b = 0; b < o->length; ++b) printf("%02x", (unsigned)(uint8_t)o->bytes[b]);
    printf(" begins=%u ready=%u published=%u released=%u\n", o->begins, o->ready, o->published, o->released);
}

static void h1nr_output_done(const H1NrObservation *o) {
    CHECK(o->begins == 1 && o->ready == 1 && o->groups == 1 && o->writes == 1);
    CHECK(o->length == o->oracle->output_bytes && !memcmp(o->bytes, o->oracle->output, o->length));
    CHECK(o->published == o->slots);
}

static void h1nr_output_unchanged(const H1NrObservation *now, const H1NrObservation *before) {
    CHECK(now->groups == before->groups && now->writes == before->writes && now->length == before->length);
    CHECK(now->begins == before->begins && now->ready == before->ready && now->published == before->published &&
        now->released == before->released && !memcmp(now->bytes, before->bytes, sizeof(now->bytes)) &&
        !memcmp(&now->observed, &before->observed, sizeof(now->observed)));
}

static void h1nr_output_prefix(const H1NrObservation *o) {
    CHECK(o->groups <= 1 && o->writes <= o->groups);
    CHECK(o->length == (o->writes ? o->oracle->output_bytes : 0));
    CHECK(!memcmp(o->bytes, o->oracle->output, o->length));
    for (size_t b = o->length; b < sizeof(o->bytes); ++b) CHECK(!o->bytes[b]);
}

static void h1nr_drive_pair(XrXirInstance *instances[2], H1NrObservation observations[2],
                          XrXirValue values[2], bool initialization) {
    bool done[2] = {false, false}; unsigned polls[2] = {0, 0};
    size_t sites[2] = {0, 0};
    while (!done[0] || !done[1]) for (unsigned i = 0; i < 2; ++i) {
        if (done[i]) continue;
        H1NrLedger peer = h1nr_ledger(instances[1 - i]);
        H1NrObservation peer_output = observations[1 - i];
        size_t begin = runtime_attempts;
        XrXirInstanceResult result = xr_xir_instance_poll_bounded(instances[i], 1);
        CHECK(++polls[i] <= 16000);
        h1nr_unchanged(instances[1 - i], &peer);
        h1nr_output_unchanged(&observations[1 - i], &peer_output);
        if (result.outcome.status == XR_XIR_CALL_READY) { sites[i] += runtime_attempts - begin; continue; }
        if (result.outcome.status != XR_XIR_CALL_RETURNED)
            fprintf(stderr, "H1 native/mixed resource file=%s initialization=%u instance=%u poll=%u status=%u state=%u\n",
                observations[i].oracle->file, (unsigned)initialization, i, polls[i],
                (unsigned)result.outcome.status, (unsigned)xr_xir_instance_state(instances[i]));
        CHECK(result.outcome.status == XR_XIR_CALL_RETURNED && xr_xir_call_result_valid(&result.outcome));
        CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_READY && result.epoch);
        h1nr_value(&result.outcome.value, initialization ? 0 : observations[i].oracle->result);
        XrXirValue occupied = {XR_XIR_I64, 0, INT64_C(54321)}, saved = occupied;
        CHECK(xr_xir_instance_take_result(instances[i], &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!memcmp(&occupied, &saved, sizeof(occupied)));
        CHECK(xr_xir_instance_take_result(instances[i], &values[i]) == XR_XIR_CALL_RETURNED);
        h1nr_value(&values[i], initialization ? 0 : observations[i].oracle->result);
        XrXirValue empty = {0};
        CHECK(xr_xir_instance_take_result(instances[i], &empty) == XR_XIR_CALL_BAD_STATE);
        CHECK(!empty.type && !empty.reserved && !empty.payload);
        h1nr_output_done(&observations[i]); h1nr_unchanged(instances[1 - i], &peer);
        h1nr_output_unchanged(&observations[1 - i], &peer_output);
        sites[i] += runtime_attempts - begin;
        done[i] = true;
        h1nr_runtime_report(observations[i].oracle, initialization ? "entry_returned" : "run_returned",
            i, instances[i], sites[i]);
    }
}

static void h1nr_domain_only(XrXirDomain *domain) {
    XrXirDomainBudgetStats d = xr_xir_domain_budget_stats(domain);
    XrXirDomainStats v = xr_xir_domain_stats(domain);
    CHECK(!d.metadata_live && !d.call_live && d.metadata_allocations == d.metadata_frees &&
        d.call_allocations == d.call_frees);
    CHECK(v.live_bytes == sizeof(*domain) && v.allocations == v.frees + 1);
}

static H1NrFixture h1nr_build(const RootParameterSourceOracle *oracle);
static void h1nr_provider_finish(bool both);

enum { H1R_CREATE, H1R_ENTRY_START, H1R_ENTRY_BODY, H1R_ENTRY_TAKE, H1R_RUN_START,
    H1R_RUN_BODY, H1R_RUN_TAKE, H1R_COPY, H1R_STOP, H1R_FREE, H1R_LAST, H1R_PHASES, H1R_AXES = 6 };
static const char *const h1r_names[H1R_PHASES] = {"create", "entry_start", "entry_body", "entry_take",
    "run_start", "run_body", "run_take", "copy", "stop", "free", "last_domain"};
typedef struct H1Witness {
    H1NrObservation output;
    XrXirInstance *instance;
    uintptr_t identity; /* Numeric address captured while the Instance is alive. */
    XrXirDomain *domain;
    XrXirInstanceConfig config;
    XrXirValue held, alias;
    XrXirInstanceResult result;
    XrXirDomainBudgetStats retired;
    size_t sites[H1R_PHASES];
    uint64_t selectors[H1R_AXES], epoch;
    unsigned state, last, exhausted;
} H1Witness;
typedef struct H1Runtime { H1NrFixture fixture; H1Witness w[2]; } H1Runtime;
typedef struct H1Minus { unsigned phase, state, groups, writes, exhausted, stop, free, retry_phase, retry_status; } H1Minus;
typedef struct H1Expected {
    size_t compiler_sites[11], sites[2][H1R_PHASES];
    uint64_t compiler_axes[3], axes[2][H1R_AXES];
    H1Minus minus[2][H1R_AXES], quota[2];
    unsigned exact_stop[2][H1R_AXES], exact_free[2][H1R_AXES];
} H1Expected;

static unsigned h1r_success(unsigned p) {
    CHECK(p < H1R_PHASES);
    if (p == H1R_ENTRY_BODY || p == H1R_ENTRY_TAKE || p == H1R_RUN_BODY || p == H1R_RUN_TAKE) return XR_XIR_CALL_RETURNED;
    return p == H1R_COPY ? (unsigned)XR_XIR_VALUE_OK : (unsigned)XR_XIR_CALL_READY;
}
static void h1r_collect(H1Witness *w) {
    if (!w->domain) return;
    XrXirDomainBudgetStats d = xr_xir_domain_budget_stats(w->domain);
    XrXirDomainStats v = xr_xir_domain_stats(w->domain); uint64_t values[H1R_AXES] = {
        d.requested_bytes, d.requested_call_bytes, v.peak_bytes, d.metadata_peak, d.call_peak, d.work};
    for (unsigned a = 0; a < H1R_AXES; ++a) if (values[a] > w->selectors[a]) w->selectors[a] = values[a];
    w->retired = d;
    if (w->instance) { w->epoch = w->instance->epoch; w->state = (unsigned)xr_xir_instance_state(w->instance); w->exhausted = (unsigned)w->instance->budget.exhausted; }
}
static void h1r_log(H1Runtime *t, unsigned i, unsigned p, unsigned status, size_t first) {
    H1Witness *w = &t->w[i]; h1r_collect(w); const XrXirDomainBudgetStats *d = &w->retired;
    printf("H1_NATIVE_RESOURCE_RUNTIME_STAGE file=%s instance=%u phase=%u name=%s first=%zu end=%zu sites=%zu status=%u "
        "owner=%p domain=%p budget=%p state=%u exhausted=%u epoch=%llu value_requested=%llu call_requested=%llu work=%llu "
        "metadata_live=%llu call_live=%llu typed=%u writes=%u physical_blocks=%zu physical_bytes=%zu\n",
        w->output.oracle->file, i, p, h1r_names[p], first, runtime_attempts, w->sites[p], status,
        (void *)w->instance, (void *)w->domain, w->instance ? (void *)&w->instance->budget : NULL,
        w->state, w->exhausted, (unsigned long long)w->epoch, (unsigned long long)d->requested_bytes,
        (unsigned long long)d->requested_call_bytes, (unsigned long long)d->work,
        (unsigned long long)d->metadata_live, (unsigned long long)d->call_live,
        w->output.groups, w->output.writes, runtime_live, runtime_bytes);
}
static void h1r_audit(H1Runtime *t, unsigned i, const char *tag) {
    H1Witness *w = &t->w[i]; h1r_collect(w); const XrXirDomainBudgetStats *d = &w->retired;
    printf("H1_NATIVE_RESOURCE_RETRY_LEDGER file=%s tag=%s instance=%u domain=%p budget=%p epoch=%llu "
        "resumes=%llu value_requested=%llu call_requested=%llu work=%llu metadata_live=%llu "
        "call_live=%llu physical_blocks=%zu physical_bytes=%zu\n", w->output.oracle->file, tag, i,
        (void *)w->domain, w->instance ? (void *)&w->instance->budget : NULL,
        (unsigned long long)w->epoch, (unsigned long long)(w->instance ? w->instance->budget.resumes : 0),
        (unsigned long long)d->requested_bytes, (unsigned long long)d->requested_call_bytes,
        (unsigned long long)d->work, (unsigned long long)d->metadata_live,
        (unsigned long long)d->call_live, runtime_live, runtime_bytes);
}
static void h1r_prepare(H1Runtime *t, H1NrFixture fixture, const RootParameterSourceOracle *oracle) {
    *t = (H1Runtime){0}; t->fixture = fixture;
    for (unsigned i = 0; i < 2; ++i) {
        H1Witness *w = &t->w[i]; w->output.oracle = oracle; w->output.module = fixture.module; w->output.slots = fixture.slots;
        w->output.sink = (XrXirOutputSink){XR_XIR_CALL_ABI_VERSION, 0, h1nr_bytes, &w->output, 65536};
        CHECK(xr_xir_instance_config_init(&w->config, sizeof(w->config)) == XR_XIR_CALL_READY);
        w->config.metadata_limit = w->config.value_limit = w->config.call_limit = 65536;
        w->config.poll_limit = 8000; w->config.depth_limit = 96;
        CHECK(w->config.requested_value_limit == UINT64_C(67108864) &&
            w->config.requested_call_limit == UINT64_C(67108864) && w->config.work_limit == UINT64_C(128000000));
        w->config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, h1nr_typed, &w->output};
        w->config.trace = h1nr_lifecycle; w->config.trace_context = &w->output;
    }
}
static void h1r_program_release(H1Runtime *t) {
    if (t->fixture.program) { xr_xir_compile_program_drop(t->fixture.program); t->fixture.program = NULL; }
}
static void h1r_no_refund(H1Witness *w, const H1NrLedger *before, XrXirDomain *domain,
    const XrXirCallBudget *budget) {
    CHECK(w->instance && w->domain == domain && &w->instance->budget == budget && budget->work_domain == domain);
    H1NrLedger after = h1nr_ledger(w->instance);
    CHECK(after.domain.requested_bytes >= before->domain.requested_bytes &&
        after.domain.requested_call_bytes >= before->domain.requested_call_bytes && after.domain.work >= before->domain.work);
    CHECK(after.domain.requested_limit == before->domain.requested_limit && after.domain.requested_call_limit == before->domain.requested_call_limit &&
        after.domain.work_limit == before->domain.work_limit && after.domain.metadata_limit == before->domain.metadata_limit &&
        after.domain.call_limit == before->domain.call_limit && after.call.requested_limit == before->call.requested_limit &&
        after.call.resume_limit == before->call.resume_limit && after.call.byte_limit == before->call.byte_limit);
    CHECK(after.call.requested_bytes >= before->call.requested_bytes && after.call.resumes >= before->call.resumes);
}
static void h1r_occupied(H1Witness *w, XrXirCallStatus expected) {
    H1NrLedger before = h1nr_ledger(w->instance); size_t first = runtime_attempts;
    XrXirValue occupied = {XR_XIR_I64, 0, INT64_C(54321)}, saved = occupied;
    CHECK(xr_xir_instance_take_result(w->instance, &occupied) == expected);
    CHECK(!memcmp(&occupied, &saved, sizeof(occupied)) && first == runtime_attempts);
    h1nr_unchanged(w->instance, &before);
}
static void h1r_failed_take(H1Witness *w) {
    CHECK(w->instance && !w->instance->budget.exhausted);
    CHECK(xr_xir_instance_state(w->instance) == (w->output.ready ? XR_XIR_INSTANCE_READY : XR_XIR_INSTANCE_FAILED));
    H1NrLedger before = h1nr_ledger(w->instance); size_t first = runtime_attempts;
    h1r_occupied(w, w->output.ready ? XR_XIR_CALL_BAD_ARGUMENT : XR_XIR_CALL_BAD_STATE);
    XrXirValue empty = {0}; CHECK(xr_xir_instance_take_result(w->instance, &empty) == XR_XIR_CALL_BAD_STATE);
    CHECK(!empty.type && !empty.reserved && !empty.payload && runtime_attempts == first);
    h1nr_unchanged(w->instance, &before);
}
static XrXirInstanceResult h1r_poll(H1Runtime *t, unsigned i) {
    H1Witness *w = &t->w[i]; XrXirInstanceResult result = {0}; unsigned polls = 0;
    do {
        H1Witness *other = &t->w[1 - i]; H1NrLedger peer = {0}; bool live = other->instance != NULL;
        if (live) peer = h1nr_ledger(other->instance);
        H1NrObservation output = other->output; result = xr_xir_instance_poll_bounded(w->instance, 1);
        CHECK(++polls <= 16000); if (live) h1nr_unchanged(other->instance, &peer);
        h1nr_output_unchanged(&other->output, &output);
    } while (result.outcome.status == XR_XIR_CALL_READY);
    CHECK(xr_xir_call_result_valid(&result.outcome)); return result;
}
static unsigned h1r_action(H1Runtime *t, unsigned i, unsigned p) {
    H1Witness *w = &t->w[i]; unsigned status = XR_XIR_CALL_BAD_STATE;
    switch (p) {
    case H1R_CREATE: {
        XrXirInstanceConfig saved; memcpy(&saved, &w->config, sizeof(saved));
        XrXirProgram *program = t->fixture.program ? t->fixture.program : t->w[1 - i].instance->program;
        status = (unsigned)xr_xir_instance_new(program, &w->config, &w->instance);
        CHECK(!memcmp(&saved, &w->config, sizeof(saved)));
        if (status == XR_XIR_CALL_READY) {
            w->domain = w->instance->domain; CHECK(xr_xir_domain_retain(w->domain)); w->output.instance = w->instance; w->identity = (uintptr_t)w->instance;
            CHECK(xr_xir_instance_state(w->instance) == XR_XIR_INSTANCE_NEW && w->instance->program == program);
        } else CHECK(!w->instance && !w->domain);
        break;
    }
    case H1R_ENTRY_START: case H1R_RUN_START:
        status = (unsigned)xr_xir_instance_start(w->instance, p == H1R_ENTRY_START ? t->fixture.entry : t->fixture.run, NULL, 0); break;
    case H1R_ENTRY_BODY: case H1R_RUN_BODY:
        w->result = h1r_poll(t, i); status = (unsigned)w->result.outcome.status;
        if (status == XR_XIR_CALL_RETURNED) {
            h1nr_value(&w->result.outcome.value, p == H1R_ENTRY_BODY ? 0 : w->output.oracle->result);
            CHECK(xr_xir_instance_state(w->instance) == XR_XIR_INSTANCE_READY); h1nr_output_prefix(&w->output);
            if (p == H1R_ENTRY_BODY) h1nr_output_done(&w->output);
        }
        break;
    case H1R_ENTRY_TAKE: case H1R_RUN_TAKE: {
        h1r_occupied(w, XR_XIR_CALL_BAD_ARGUMENT); XrXirValue value = {0};
        status = (unsigned)xr_xir_instance_take_result(w->instance, &value);
        if (status == XR_XIR_CALL_RETURNED) {
            h1nr_value(&value, p == H1R_ENTRY_TAKE ? 0 : w->output.oracle->result);
            XrXirValue empty = {0}; CHECK(xr_xir_instance_take_result(w->instance, &empty) == XR_XIR_CALL_BAD_STATE);
            CHECK(!empty.type && !empty.reserved && !empty.payload);
            if (p == H1R_ENTRY_TAKE) xr_xir_value_drop(&value); else w->held = value;
        } else CHECK(!value.type && !value.reserved && !value.payload);
        break;
    }
    case H1R_COPY: {
        H1NrLedger before = h1nr_ledger(w->instance); XrXirValue saved = w->held;
        status = (unsigned)xr_xir_value_copy(&w->held, &w->alias);
        CHECK(!memcmp(&saved, &w->held, sizeof(saved))); h1nr_unchanged(w->instance, &before);
        if (status == XR_XIR_VALUE_OK) h1nr_value(&w->alias, w->output.oracle->result);
        else CHECK(!w->alias.type && !w->alias.reserved && !w->alias.payload); break;
    }
    case H1R_STOP: status = (unsigned)xr_xir_instance_stop(w->instance); break;
    case H1R_FREE: {
        XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(w->domain); h1r_collect(w);
        status = (unsigned)xr_xir_instance_free(w->instance); w->instance = NULL; w->output.instance = NULL;
        XrXirDomainBudgetStats after = xr_xir_domain_budget_stats(w->domain);
        CHECK(after.requested_bytes == before.requested_bytes && after.requested_call_bytes == before.requested_call_bytes && after.work >= before.work);
        h1nr_domain_only(w->domain); w->state = XR_XIR_INSTANCE_DRAINING;
        CHECK(w->output.released == w->output.published); break;
    }
    case H1R_LAST:
        CHECK(!w->instance && w->domain); h1nr_domain_only(w->domain);
        if (w->held.type) h1nr_value(&w->held, w->output.oracle->result);
        if (w->alias.type) h1nr_value(&w->alias, w->output.oracle->result);
        printf("H1_NATIVE_RESOURCE_OWNED file=%s instance=%u owner=0x%" PRIxPTR " held_type=%u held_value=%lld alias_type=%u alias_value=%lld\n",
            w->output.oracle->file, i, w->identity, w->held.type, (long long)w->held.payload,
            w->alias.type, (long long)w->alias.payload);
        xr_xir_value_drop(&w->held); xr_xir_value_drop(&w->alias); h1r_collect(w);
        CHECK(!w->held.type && !w->held.reserved && !w->held.payload && !w->alias.type && !w->alias.reserved && !w->alias.payload);
        xr_xir_domain_drop(w->domain); w->domain = NULL; status = XR_XIR_CALL_READY; break;
    default: CHECK(false); break;
    }
    return status;
}
static unsigned h1r_step(H1Runtime *t, unsigned i, unsigned p) {
    H1Witness *other = &t->w[1 - i]; bool live = other->instance != NULL;
    H1NrLedger peer = {0}; if (live) peer = h1nr_ledger(other->instance);
    H1NrObservation output = other->output; size_t first = runtime_attempts;
    unsigned status = h1r_action(t, i, p); CHECK(runtime_attempts >= first);
    t->w[i].sites[p] = runtime_attempts - first; t->w[i].last = p;
    if (live) h1nr_unchanged(other->instance, &peer);
    h1nr_output_unchanged(&other->output, &output); h1r_log(t, i, p, status, first);
    if (p == H1R_ENTRY_TAKE || p == H1R_RUN_TAKE || p >= H1R_COPY) CHECK(first == runtime_attempts);
    return status;
}
static void h1r_forward(H1Runtime *t, unsigned i, unsigned first, unsigned last) {
    for (unsigned p = first; p <= last; ++p) CHECK(h1r_step(t, i, p) == h1r_success(p));
}
static void h1r_final(H1Runtime *t, unsigned target, unsigned stop, unsigned freed) {
    bool both = t->w[target].held.type != XR_XIR_UNIT;
    h1nr_output_done(&t->w[1 - target].output);
    h1r_program_release(t);
    for (unsigned k = 0; k < 2; ++k) {
        unsigned i = k ? 1 - target : target; H1Witness *w = &t->w[i];
        if (w->instance) {
            if (w->last < H1R_STOP) CHECK(h1r_step(t, i, H1R_STOP) == (i == target ? stop : (unsigned)XR_XIR_CALL_READY));
            CHECK(h1r_step(t, i, H1R_FREE) == (i == target ? freed : (unsigned)XR_XIR_CALL_READY));
        }
    }
    instance_compile_zero();
    for (unsigned i = 0; i < 2; ++i) if (t->w[i].domain) CHECK(h1r_step(t, i, H1R_LAST) == XR_XIR_CALL_READY);
    CHECK(!runtime_live && !runtime_bytes); instance_compile_zero();
    printf("H1_NATIVE_RESOURCE_PHYSICAL file=%s compiler=0/0 runtime=0/0\n", t->w[0].output.oracle->file);
    h1nr_provider_finish(both);
    for (unsigned i = 0; i < 2; ++i) h1nr_output_report(&t->w[i].output, i, t->w[i].identity);
}
static void h1r_normal(H1Runtime *t, H1NrFixture fixture, const RootParameterSourceOracle *oracle) {
    h1r_prepare(t, fixture, oracle);
    for (unsigned p = 0; p < H1R_PHASES; ++p) {
        if (p == H1R_ENTRY_START) h1r_program_release(t);
        for (unsigned i = 0; i < 2; ++i) CHECK(h1r_step(t, i, p) == h1r_success(p));
        if (p == H1R_CREATE) CHECK(t->w[0].instance != t->w[1].instance && t->w[0].domain != t->w[1].domain &&
            &t->w[0].instance->budget != &t->w[1].instance->budget && t->w[0].instance->program == t->w[1].instance->program);
        if (p == H1R_ENTRY_TAKE) {
            CHECK(t->w[0].instance->function_gate != t->w[1].instance->function_gate);
            if (fixture.slots) for (unsigned i = 0; i < 2; ++i) h1nr_value(&t->w[i].instance->slots[0], 9);
        }
    }
    CHECK(!runtime_live && !runtime_bytes); instance_compile_zero();
    h1nr_provider_finish(true);
    for (unsigned i = 0; i < 2; ++i) {
        H1Witness *w = &t->w[i]; h1nr_output_done(&w->output);
        h1nr_output_report(&w->output, i, w->identity);
        printf("H1_NATIVE_RESOURCE_CENSUS file=%s instance=%u sites=", oracle->file, i);
        for (unsigned p = 0; p < H1R_PHASES; ++p) printf("%s%zu", p ? "," : "", w->sites[p]);
        printf(" selectors="); for (unsigned a = 0; a < H1R_AXES; ++a) printf("%s%llu", a ? "," : "", (unsigned long long)w->selectors[a]);
        printf(" compiler_physical=0/0 runtime_physical=0/0\n");
    }
}
static void h1r_selector(H1Witness *w, unsigned axis, uint64_t amount) {
    CHECK(axis < H1R_AXES && amount);
    if (axis == 0) w->config.requested_value_limit = amount;
    else if (axis == 1) w->config.requested_call_limit = amount;
    else if (axis == 2) w->config.value_limit = amount;
    else if (axis == 3) w->config.metadata_limit = amount;
    else if (axis == 4) w->config.call_limit = amount;
    else w->config.work_limit = amount;
}
static void h1r_sticky(H1Runtime *t, unsigned i, XrXirCallStatus reason) {
    H1Witness *w = &t->w[i]; H1NrLedger before = h1nr_ledger(w->instance);
    H1NrObservation own_output = w->output;
    size_t first = runtime_attempts; uint64_t epoch = w->instance->epoch;
    H1Witness *peer = &t->w[1 - i]; H1NrLedger peer_fees = h1nr_ledger(peer->instance);
    H1NrObservation peer_output = peer->output;
    CHECK(xr_xir_instance_state(w->instance) == XR_XIR_INSTANCE_FAILED && !w->output.ready);
    for (unsigned k = 0; k < 2; ++k) {
        CHECK(xr_xir_instance_start(w->instance, t->fixture.entry, NULL, 0) == reason);
        XrXirInstanceResult again = xr_xir_instance_poll_bounded(w->instance, 1);
        CHECK(again.outcome.status == reason && again.epoch == epoch && xr_xir_call_result_valid(&again.outcome));
        h1r_occupied(w, XR_XIR_CALL_BAD_STATE);
        CHECK(first == runtime_attempts); h1nr_unchanged(w->instance, &before);
        h1nr_output_unchanged(&w->output, &own_output);
        h1nr_unchanged(peer->instance, &peer_fees); h1nr_output_unchanged(&peer->output, &peer_output);
    }
}
static void h1r_retry(H1Runtime *t, unsigned i, unsigned p, const H1NrLedger *s0,
    XrXirDomain *domain, const XrXirCallBudget *budget) {
    H1Witness *w = &t->w[i]; h1r_no_refund(w, s0, domain, budget);
    CHECK(!budget->exhausted);
    if ((p == H1R_ENTRY_BODY && !w->output.ready)) { h1r_sticky(t, i, XR_XIR_CALL_OOM); return; }
    CHECK(xr_xir_instance_state(w->instance) == (p == H1R_ENTRY_START ? XR_XIR_INSTANCE_NEW : XR_XIR_INSTANCE_READY));
    unsigned first = p == H1R_ENTRY_START ? H1R_ENTRY_START : H1R_RUN_START;
    H1NrObservation output = w->output;
    h1r_forward(t, i, first, H1R_COPY); h1r_no_refund(w, s0, domain, budget);
    if (first == H1R_RUN_START) h1nr_output_unchanged(&w->output, &output);
    else h1nr_output_done(&w->output);
    printf("H1_NATIVE_RESOURCE_RETRY instance=%u same_Instance=1 same_Domain=1 same_CallBudget=1 fees_retained=1\n", i);
}
static void h1r_oom(const RootParameterSourceOracle *oracle, const H1Expected *e, unsigned target, unsigned p, size_t first, size_t count) {
    CHECK(target < 2 && p < H1R_PHASES && count <= 64);
    for (unsigned i = target; i <= target; ++i) {
        CHECK(first <= e->sites[i][p] && count <= e->sites[i][p] - first);
        for (size_t point = first; point < first + count; ++point) {
            H1Runtime t; h1r_prepare(&t, h1nr_build(oracle), oracle); unsigned peer = 1 - i;
            CHECK(h1r_step(&t, peer, H1R_CREATE) == XR_XIR_CALL_READY);
            if (p != H1R_CREATE) h1r_forward(&t, i, H1R_CREATE, p - 1);
            H1Witness *w = &t.w[i]; H1NrLedger s0 = {0}; XrXirDomain *domain = w->domain;
            const XrXirCallBudget *budget = w->instance ? &w->instance->budget : NULL;
            uint64_t epoch = w->instance ? w->instance->epoch : 0;
            if (w->instance) s0 = h1nr_ledger(w->instance);
            if (p == H1R_ENTRY_BODY || p == H1R_RUN_BODY) h1r_occupied(w, XR_XIR_CALL_BAD_STATE);
            h1r_audit(&t, i, "S0"); size_t before = runtime_attempts, blocks = runtime_live, bytes = runtime_bytes;
            CHECK(point < SIZE_MAX - before); runtime_fail_at = before + point;
            unsigned status = h1r_step(&t, i, p); runtime_fail_at = SIZE_MAX;
            CHECK(status == XR_XIR_CALL_OOM && runtime_attempts == before + point + 1); h1r_audit(&t, i, "S1");
            if (p == H1R_CREATE) {
                CHECK(!w->instance && !w->domain && runtime_live == blocks && runtime_bytes == bytes);
                h1r_forward(&t, i, H1R_CREATE, H1R_COPY);
                puts("H1_NATIVE_RESOURCE_RETRY same_Program=1 same_Instance=0 same_Domain=0");
            } else {
                h1r_no_refund(w, &s0, domain, budget);
                H1NrLedger s1 = h1nr_ledger(w->instance); CHECK(s1.domain.work > s0.domain.work);
                if (p == H1R_ENTRY_START || p == H1R_RUN_START) {
                    CHECK(s1.domain.requested_call_bytes > s0.domain.requested_call_bytes && s1.call.requested_bytes > s0.call.requested_bytes);
                    CHECK(s1.domain.metadata_live == s0.domain.metadata_live && s1.domain.call_live == s0.domain.call_live &&
                        s1.values.live_bytes == s0.values.live_bytes && s1.call.live_bytes == s0.call.live_bytes);
                }
                CHECK(!w->held.type && !w->alias.type);
                if (p == H1R_ENTRY_START || p == H1R_RUN_START) CHECK(w->instance->epoch == epoch && budget->resumes == s0.call.resumes);
                if (p == H1R_ENTRY_BODY || p == H1R_RUN_BODY) {
                    CHECK(xr_xir_instance_state(w->instance) == (w->output.ready ? XR_XIR_INSTANCE_READY : XR_XIR_INSTANCE_FAILED));
                    h1r_failed_take(w);
                    bool renderer = w->output.renderer_end && before + point == w->output.renderer_begin;
                    CHECK(w->output.renderer_oom == (unsigned)renderer);
                    if (w->output.renderer_oom) CHECK(w->output.groups == 1 && !w->output.writes && !w->output.length);
                }
                h1r_retry(&t, i, p, &s0, domain, budget);
            }
            h1r_audit(&t, i, "S2"); h1r_program_release(&t); h1r_forward(&t, peer, H1R_ENTRY_START, H1R_COPY);
            h1r_final(&t, i, XR_XIR_CALL_READY, XR_XIR_CALL_READY);
            printf("H1_NATIVE_RESOURCE_RUNTIME_OOM file=%s instance=%u phase=%u point=%zu denominator=%zu physical=0/0,0/0\n",
                oracle->file, i, p, point, e->sites[i][p]);
        }
    }
    printf("H1_NATIVE_RESOURCE_RUNTIME_RANGE file=%s instance=%u phase=%u first=%zu count=%zu denominator=%zu complete=1\n",
        oracle->file, target, p, first, count, e->sites[target][p]);
}
static void h1r_budget_retry(H1Runtime *t, unsigned i, const H1Minus *m) {
    H1Witness *w = &t->w[i];
    if (!w->instance) { CHECK(m->retry_phase == H1R_PHASES); return; }
    if (w->state == XR_XIR_INSTANCE_FAILED) {
        CHECK(m->retry_phase == H1R_PHASES); h1r_sticky(t, i, XR_XIR_CALL_LIMIT); return;
    }
    if (w->state == XR_XIR_INSTANCE_DRAINING) { CHECK(m->retry_phase == H1R_PHASES); return; }
    CHECK(m->retry_phase == H1R_RUN_START || m->retry_phase == H1R_RUN_BODY);
    H1NrLedger before = h1nr_ledger(w->instance), peer = h1nr_ledger(t->w[1 - i].instance);
    H1NrObservation output = t->w[1 - i].output;
    XrXirDomain *domain = w->domain; const XrXirCallBudget *budget = &w->instance->budget;
    uint64_t epoch = w->instance->epoch, resumes = budget->resumes;
    bool exhausted = budget->exhausted;
    h1r_audit(t, i, "budget_retry_S0");
    unsigned started = h1r_step(t, i, H1R_RUN_START);
    CHECK(started == (m->retry_phase == H1R_RUN_START ? m->retry_status : (unsigned)XR_XIR_CALL_READY));
    if (exhausted) CHECK(started != XR_XIR_CALL_READY && m->retry_phase == H1R_RUN_START);
    if (started != XR_XIR_CALL_READY) CHECK(w->instance->epoch == epoch && budget->resumes == resumes);
    if (m->retry_phase == H1R_RUN_BODY) CHECK(h1r_step(t, i, H1R_RUN_BODY) == m->retry_status);
    h1r_no_refund(w, &before, domain, budget); h1nr_unchanged(t->w[1 - i].instance, &peer);
    h1nr_output_unchanged(&t->w[1 - i].output, &output); h1r_audit(t, i, "budget_retry_S1");
}
static void h1r_axes(const RootParameterSourceOracle *oracle, const H1Expected *e) {
    for (unsigned i = 0; i < 2; ++i) for (unsigned a = 0; a < H1R_AXES; ++a) for (unsigned minus = 0; minus < 2; ++minus) {
        H1Runtime t; h1r_prepare(&t, h1nr_build(oracle), oracle); unsigned peer = 1 - i;
        H1Witness *w = &t.w[i]; const H1Minus *m = &e->minus[i][a];
        h1r_selector(w, a, e->axes[i][a] - minus); CHECK(h1r_step(&t, peer, H1R_CREATE) == XR_XIR_CALL_READY);
        unsigned last = minus ? m->phase : H1R_FREE;
        for (unsigned p = H1R_CREATE; p <= last; ++p) {
            unsigned status = h1r_step(&t, i, p);
            unsigned expected = minus && p == last ? (unsigned)XR_XIR_CALL_LIMIT : h1r_success(p);
            if (!minus && p == H1R_STOP) expected = e->exact_stop[i][a];
            if (!minus && p == H1R_FREE) expected = e->exact_free[i][a];
            CHECK(status == expected);
        }
        if (minus) {
            CHECK(w->state == m->state && w->output.groups == m->groups && w->output.writes == m->writes);
            if (w->instance) CHECK((unsigned)w->instance->budget.exhausted == m->exhausted);
            h1r_budget_retry(&t, i, m);
        } else for (unsigned axis = 0; axis < H1R_AXES; ++axis) CHECK(w->selectors[axis] == e->axes[i][axis]);
        h1r_program_release(&t); h1r_forward(&t, peer, H1R_ENTRY_START, H1R_COPY);
        h1r_final(&t, i, minus ? m->stop : e->exact_stop[i][a], minus ? m->free : e->exact_free[i][a]);
        printf("H1_NATIVE_RESOURCE_RUNTIME_AXIS file=%s instance=%u axis=%u minus1=%u cap=%llu physical=0/0,0/0\n",
            oracle->file, i, a, minus, (unsigned long long)(e->axes[i][a] - minus));
    }
}
static void h1r_same_quota(const RootParameterSourceOracle *oracle, const H1Expected *e) {
    for (unsigned i = 0; i < 2; ++i) {
        H1Runtime t; h1r_prepare(&t, h1nr_build(oracle), oracle); unsigned peer = 1 - i;
        H1Witness *w = &t.w[i]; const H1Minus *m = &e->quota[i]; h1r_selector(w, 5, e->axes[i][5]);
        CHECK(h1r_step(&t, peer, H1R_CREATE) == XR_XIR_CALL_READY);
        h1r_forward(&t, i, H1R_CREATE, H1R_ENTRY_TAKE); h1r_program_release(&t);
        H1NrLedger s0 = h1nr_ledger(w->instance); XrXirDomain *domain = w->domain;
        const XrXirCallBudget *budget = &w->instance->budget; uint64_t epoch = w->instance->epoch;
        h1r_audit(&t, i, "samequota_S0"); size_t first = runtime_attempts; runtime_fail_at = first;
        CHECK(h1r_step(&t, i, H1R_RUN_START) == XR_XIR_CALL_OOM && runtime_attempts == first + 1);
        runtime_fail_at = SIZE_MAX; H1NrLedger s1 = h1nr_ledger(w->instance);
        CHECK(w->instance->epoch == epoch && !budget->exhausted && s1.domain.work == s0.domain.work + 1);
        CHECK(s1.domain.requested_call_bytes > s0.domain.requested_call_bytes && s1.call.requested_bytes > s0.call.requested_bytes);
        CHECK(s1.call.live_bytes == s0.call.live_bytes && s1.domain.call_live == s0.domain.call_live);
        h1r_no_refund(w, &s0, domain, budget); h1r_audit(&t, i, "samequota_S1");
        CHECK(m->phase >= H1R_RUN_START && m->phase <= H1R_FREE && m->phase != H1R_RUN_TAKE && m->phase != H1R_COPY);
        for (unsigned p = H1R_RUN_START; p <= m->phase; ++p)
            CHECK(h1r_step(&t, i, p) == (p == m->phase ? (unsigned)XR_XIR_CALL_LIMIT : h1r_success(p)));
        CHECK(w->state == m->state && w->output.groups == m->groups && w->output.writes == m->writes);
        if (w->instance) { CHECK((unsigned)budget->exhausted == m->exhausted); h1r_no_refund(w, &s1, domain, budget); }
        h1r_audit(&t, i, "samequota_S2"); h1r_forward(&t, peer, H1R_ENTRY_START, H1R_COPY);
        h1r_final(&t, i, m->stop, m->free);
        printf("H1_NATIVE_RESOURCE_SAMEQUOTA file=%s instance=%u first=OOM retained_work=1 retry=LIMIT refund=0 physical=0/0,0/0\n", oracle->file, i);
    }
}
static void h1r_typed_failure(const RootParameterSourceOracle *oracle, bool limit) {
    for (unsigned i = 0; i < 2; ++i) {
        H1Runtime t; h1r_prepare(&t, h1nr_build(oracle), oracle); unsigned peer = 1 - i;
        h1r_forward(&t, i, H1R_CREATE, H1R_ENTRY_START); CHECK(h1r_step(&t, peer, H1R_CREATE) == XR_XIR_CALL_READY);
        H1Witness *w = &t.w[i]; w->output.byte_failure = limit ? XR_XIR_OUTPUT_LIMIT : XR_XIR_OUTPUT_ERROR;
        unsigned expected = limit ? (unsigned)XR_XIR_CALL_LIMIT : (unsigned)XR_XIR_CALL_OUTPUT_ERROR;
        CHECK(h1r_step(&t, i, H1R_ENTRY_BODY) == expected);
        CHECK(w->output.groups == 1 && !w->output.writes && !w->output.length && !w->output.renderer_oom);
        CHECK(w->output.renderer_end == w->output.renderer_begin + 1);
        if (!w->output.ready) h1r_sticky(&t, i, (XrXirCallStatus)expected);
        else CHECK(xr_xir_instance_state(w->instance) == XR_XIR_INSTANCE_READY);
        h1r_failed_take(w);
        h1r_program_release(&t); h1r_forward(&t, peer, H1R_ENTRY_START, H1R_COPY);
        h1r_final(&t, i, XR_XIR_CALL_READY, XR_XIR_CALL_READY);
        printf("H1_NATIVE_RESOURCE_TYPED_FAILURE file=%s instance=%u status=%u typed=1 bytes_published=0 physical=0/0,0/0\n", oracle->file, i, expected);
    }
}
#endif
