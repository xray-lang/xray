/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_conditional_retry_cases.h - Complete operations on retained finite owners
 */
#ifndef XIR_CONDITIONAL_RETRY_CASES_H
#define XIR_CONDITIONAL_RETRY_CASES_H

typedef struct ConditionalRetryTrial {
    uint32_t which;
    size_t ordinal, sites;
    XrCompileResources *resources;
    XrCompileResourceStats initial, single;
    RootParameterMark retained;
    XrXirStatus first;
} ConditionalRetryTrial;

static XrXirStatus conditional_retry_case(const XrXirCompileContext *context,uint32_t which);
static bool conditional_retry_reported[72];
static uint64_t conditional_retry_counts[72];

/* The original single successful operation supplies the minimum complete
 * cost. Nothing resets the retained ledger or resumes a successful substage. */
static void conditional_retry_operation(const XrXirCompileContext *context,
    const ConditionalRetryTrial *trial) {
    CHECK(trial->which<72 && context->resources==trial->resources);
    XrCompileResourceStats consumed=rp_stats(context),initial=trial->initial,single=trial->single;
    CHECK(consumed.live_bytes==initial.live_bytes && single.live_bytes==initial.live_bytes);
    CHECK(single.work>=initial.work && single.allocated_bytes>=initial.allocated_bytes &&
        single.allocation_count>=initial.allocation_count);
    rp_balanced(trial->retained);
    XrCompileResourceLimits limits=context->resources->limits;
    CHECK(consumed.work<=limits.work && consumed.allocated_bytes<=limits.allocated_bytes);
    uint64_t work=single.work-initial.work,bytes=single.allocated_bytes-initial.allocated_bytes;
    uint64_t allocations=single.allocation_count-initial.allocation_count;
    uint64_t work_left=limits.work-consumed.work,bytes_left=limits.allocated_bytes-consumed.allocated_bytes;
    bool fits=work<=work_left && bytes<=bytes_left && single.peak_bytes<=limits.live_bytes;
    if (trial->first==XR_XIR_BUDGET) CHECK(!fits);
    size_t attempts=rp_attempts;rp_fail_at=SIZE_MAX;
    XrXirStatus status=conditional_retry_case(context,trial->which);
    XrCompileResourceStats retried=rp_stats(context);
    CHECK(context->resources==trial->resources && retried.live_bytes==initial.live_bytes &&
        retried.work>=consumed.work && retried.allocated_bytes>=consumed.allocated_bytes &&
        retried.allocation_count>=consumed.allocation_count && rp_attempts>=attempts);
    rp_balanced(trial->retained);
    if (!fits && !conditional_retry_reported[trial->which]) {
        fprintf(stderr,"CONDITIONAL_RETRY_REMAINDER case=%u ordinal=%zu first=%u result=%u "
            "required_work=%llu remaining_work=%llu required_bytes=%llu remaining_bytes=%llu "
            "required_peak=%llu live_limit=%llu consumed_work=%llu retry_work=%llu\n",
            trial->which,trial->ordinal,(uint32_t)trial->first,(uint32_t)status,
            (unsigned long long)work,(unsigned long long)work_left,(unsigned long long)bytes,
            (unsigned long long)bytes_left,(unsigned long long)single.peak_bytes,
            (unsigned long long)limits.live_bytes,(unsigned long long)consumed.work,
            (unsigned long long)retried.work);
        conditional_retry_reported[trial->which]=true;
    }
    if (status!=(fits?XR_XIR_OK:XR_XIR_BUDGET)) fprintf(stderr,
        "CONDITIONAL_RETRY_FIRST case=%u ordinal=%zu first=%u result=%u fits=%u "
        "single_work=%llu consumed_work=%llu retried_work=%llu\n",trial->which,trial->ordinal,
        (uint32_t)trial->first,(uint32_t)status,(uint32_t)fits,(unsigned long long)work,
        (unsigned long long)consumed.work,(unsigned long long)retried.work);
    CHECK(status==(fits?XR_XIR_OK:XR_XIR_BUDGET));
    if (fits) {
        CHECK(retried.work-consumed.work==work && retried.allocated_bytes-consumed.allocated_bytes==bytes &&
            retried.allocation_count-consumed.allocation_count==allocations);
        CHECK(rp_attempts-attempts==trial->sites);
    }
    ++conditional_retry_counts[trial->which];
}

/* Every case runs once before any retry. This short observer cannot
 * replace the original all-ordinal and resource-boundary registrations. */
static void conditional_retry_census(void) {
    for (uint32_t which=0;which<72;++which) {
        RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_injected=false;
        XrXirCompileContext context=rp_owner(rp_caps());XrCompileResourceStats initial=rp_stats(&context);
        RootParameterMark retained=rp_mark();size_t attempts=rp_attempts;
        XrXirStatus status=conditional_retry_case(&context,which);XrCompileResourceStats single=rp_stats(&context);
        fprintf(stderr,"CONDITIONAL_RETRY_CENSUS case=%u status=%u sites=%zu "
            "initial_allocations=%llu allocations=%llu initial_bytes=%llu bytes=%llu "
            "initial_work=%llu work=%llu live=%llu peak=%llu\n",which,(uint32_t)status,rp_attempts-attempts,
            (unsigned long long)initial.allocation_count,(unsigned long long)single.allocation_count,
            (unsigned long long)initial.allocated_bytes,(unsigned long long)single.allocated_bytes,
            (unsigned long long)initial.work,(unsigned long long)single.work,
            (unsigned long long)single.live_bytes,(unsigned long long)single.peak_bytes);
        CHECK(status==XR_XIR_OK && single.live_bytes==initial.live_bytes);rp_balanced(retained);
        rp_owner_free(&context,initial.live_bytes);rp_balanced(physical);
    }
}

static void conditional_retry_complete(void) {
    for (uint32_t which=0;which<72;++which) CHECK(conditional_retry_counts[which]>=6);
}
#endif // XIR_CONDITIONAL_RETRY_CASES_H
