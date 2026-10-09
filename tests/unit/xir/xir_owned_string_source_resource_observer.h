/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_owned_string_source_resource_observer.h - Independent output and fee witnesses
 *
 * KEY CONCEPT:
 *   Observe real typed output and allocator differences without granting authority.
 */
#ifndef XIR_OWNED_STRING_SOURCE_RESOURCE_OBSERVER_H
#define XIR_OWNED_STRING_SOURCE_RESOURCE_OBSERVER_H
#include "xir/xxir_output.h"

enum { CS_INSTANCES = 2, CS_COMPILER_PHASES = 10, CS_RUNTIME_PHASES = 10, CS_SELECTORS = 6 };
enum { CS_CREATE, CS_MAIN_START, CS_MAIN_BODY, CS_MAIN_TAKE, CS_OWNED_START,
    CS_OWNED_BODY, CS_OWNED_TAKE, CS_COPY, CS_CLOSE, CS_LAST_ALIAS };
static const char *const cs_runtime_names[CS_RUNTIME_PHASES] = {
    "create", "main_start", "main_body", "main_take", "owned_start", "owned_body",
    "owned_take", "alias_copy", "instance_close", "last_alias_release"};
static const char *const cs_compiler_names[CS_COMPILER_PHASES] = {
    "owner_and_source_io", "session", "source_check", "checked_write", "checked_read",
    "specialize", "reverify", "lower", "vm_bind", "program_seal"};
static const char cs_result_bytes[] = "root-owned-result";
static const char cs_output_bytes[] = "C2 42 true root-owned-result\n";

typedef struct CsObservation {
    unsigned groups, writes, begins, ready, published, released;
    uint32_t module;
    char bytes[64];
    size_t length;
    XrXirOutputSink sink;
} CsObservation;

typedef struct CsLedger {
    XrXirDomainBudgetStats domain;
    XrXirDomainStats values;
    uint64_t call_requested, call_live, call_peak, call_allocations, call_frees;
    uint64_t call_resumes, call_transitions;
} CsLedger;

typedef struct CsMetrics {
    size_t compiler_sites[CS_COMPILER_PHASES];
    XrCompileResourceStats compiler;
    size_t sites[CS_INSTANCES][CS_RUNTIME_PHASES];
    uint64_t boundary[CS_INSTANCES][CS_SELECTORS];
    uint64_t fees_after_close[CS_INSTANCES][3];
} CsMetrics;

static void cs_expect_string(const XrXirValue *value) {
    const char *bytes = NULL;
    size_t length = 0;
    CHECK(value->type == XR_XIR_STRING && !value->reserved);
    CHECK(xr_xir_string_view(value, &bytes, &length));
    CHECK(length == sizeof(cs_result_bytes) - 1 && !memcmp(bytes, cs_result_bytes, length));
    CHECK(xr_xir_value_arena(value) == NULL);
}

static XrXirOutputStatus cs_output_write(void *opaque, XrXirOutputStream stream,
                                       const char *bytes, size_t length) {
    CsObservation *w = opaque;
    CHECK(stream == XR_XIR_STDOUT && !w->writes);
    CHECK(length == sizeof(cs_output_bytes) - 1 && !memcmp(bytes, cs_output_bytes, length));
    CHECK(length <= sizeof(w->bytes));
    memcpy(w->bytes, bytes, length);
    w->length = length;
    ++w->writes;
    return XR_XIR_OUTPUT_OK;
}

static XrXirOutputStatus cs_typed_output(void *opaque, const XrXirOutputGroup *group) {
    CsObservation *w = opaque;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->count == 4);
    CHECK(group->values && !w->groups);
    const XrXirValue *v = group->values;
    const char *bytes = NULL;
    size_t length = 0;
    CHECK(v[0].type == XR_XIR_STRING && !v[0].reserved);
    CHECK(xr_xir_string_view(&v[0], &bytes, &length) && length == 2 && !memcmp(bytes, "C2", 2));
    CHECK(v[1].type == XR_XIR_I64 && !v[1].reserved && v[1].payload == 42);
    CHECK(v[2].type == XR_XIR_BOOL && !v[2].reserved && v[2].payload == 1);
    cs_expect_string(&v[3]);
    ++w->groups;
    return xr_xir_output_render(&w->sink, group);
}

static void cs_lifecycle(void *opaque, XrXirLifecycleEvent event, uint32_t index) {
    CsObservation *w = opaque;
    CHECK(index == w->module);
    if (event == XR_XIR_MODULE_BEGIN) CHECK(++w->begins == 1 && !w->ready);
    else if (event == XR_XIR_MODULE_READY) CHECK(w->begins == 1 && ++w->ready == 1);
    else {
        if (event == XR_XIR_SLOT_PUBLISHED) ++w->published;
        else { CHECK(event == XR_XIR_SLOT_RELEASED); ++w->released; }
        CHECK(false); /* This fixture has no module slots. */
    }
}

static CsLedger cs_ledger(const XrXirInstance *instance) {
    CHECK(instance && instance->domain && instance->budget.work_domain == instance->domain);
    const XrXirCallBudget *b = &instance->budget;
    return (CsLedger){xr_xir_domain_budget_stats(instance->domain), xr_xir_domain_stats(instance->domain),
        b->requested_bytes, b->live_bytes, b->peak_bytes, b->allocations, b->frees,
        b->resumes, b->transitions};
}

static void cs_print_ledger(unsigned i, const char *phase, XrXirDomain *domain,
                            const XrXirDomainBudgetStats *d, const XrXirDomainStats *v) {
    printf("C2_LEDGER instance=%u phase=%s domain=%p requested_value=%llu requested_call=%llu "
        "work=%llu value_live=%llu value_peak=%llu metadata_live=%llu metadata_peak=%llu "
        "call_live=%llu call_peak=%llu value_alloc=%llu value_free=%llu "
        "metadata_alloc=%llu metadata_free=%llu call_alloc=%llu call_free=%llu\n", i, phase,
        (void *)domain, (unsigned long long)d->requested_bytes,
        (unsigned long long)d->requested_call_bytes, (unsigned long long)d->work,
        (unsigned long long)v->live_bytes, (unsigned long long)v->peak_bytes,
        (unsigned long long)d->metadata_live, (unsigned long long)d->metadata_peak,
        (unsigned long long)d->call_live, (unsigned long long)d->call_peak,
        (unsigned long long)v->allocations, (unsigned long long)v->frees,
        (unsigned long long)d->metadata_allocations, (unsigned long long)d->metadata_frees,
        (unsigned long long)d->call_allocations, (unsigned long long)d->call_frees);
}

static void cs_record_boundary(CsMetrics *metrics, unsigned i, const CsLedger *s) {
    metrics->boundary[i][0] = s->domain.requested_bytes;
    metrics->boundary[i][1] = s->domain.requested_call_bytes;
    metrics->boundary[i][2] = s->values.peak_bytes;
    metrics->boundary[i][3] = s->domain.metadata_peak;
    metrics->boundary[i][4] = s->domain.call_peak;
    metrics->boundary[i][5] = s->domain.work;
    CHECK(s->call_requested <= s->domain.requested_call_bytes);
    CHECK(s->call_peak <= s->domain.call_peak);
}

static void cs_unchanged_other(const XrXirInstance *other, const CsLedger *before) {
    CsLedger after = cs_ledger(other);
    CHECK(after.domain.requested_bytes == before->domain.requested_bytes);
    CHECK(after.domain.requested_call_bytes == before->domain.requested_call_bytes);
    CHECK(after.domain.requested_limit == before->domain.requested_limit);
    CHECK(after.domain.requested_call_limit == before->domain.requested_call_limit);
    CHECK(after.domain.work == before->domain.work && after.domain.work_limit == before->domain.work_limit);
    CHECK(after.domain.bound == before->domain.bound && after.domain.metadata_limit == before->domain.metadata_limit);
    CHECK(after.domain.metadata_live == before->domain.metadata_live && after.domain.metadata_peak == before->domain.metadata_peak);
    CHECK(after.domain.metadata_allocations == before->domain.metadata_allocations && after.domain.metadata_frees == before->domain.metadata_frees);
    CHECK(after.domain.call_limit == before->domain.call_limit && after.domain.call_live == before->domain.call_live);
    CHECK(after.domain.call_peak == before->domain.call_peak && after.domain.call_allocations == before->domain.call_allocations);
    CHECK(after.domain.call_frees == before->domain.call_frees);
    CHECK(after.values.live_bytes == before->values.live_bytes && after.values.peak_bytes == before->values.peak_bytes);
    CHECK(after.values.allocations == before->values.allocations && after.values.frees == before->values.frees);
    CHECK(after.values.reallocations == before->values.reallocations);
    CHECK(after.call_requested == before->call_requested && after.call_live == before->call_live);
    CHECK(after.call_peak == before->call_peak && after.call_allocations == before->call_allocations);
    CHECK(after.call_frees == before->call_frees && after.call_resumes == before->call_resumes);
    CHECK(after.call_transitions == before->call_transitions);
}
#endif // XIR_OWNED_STRING_SOURCE_RESOURCE_OBSERVER_H
