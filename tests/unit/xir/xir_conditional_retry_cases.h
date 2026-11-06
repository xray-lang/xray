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
/* The original census owns cases 0..99; nonroot Input proofs own 100..103. */
/* This is the test host's execution read policy, not compiler admission. */
typedef struct ConditionalExecutionPolicy {
    uint64_t remaining,calls;
    bool refused;
} ConditionalExecutionPolicy;
static bool conditional_execution_take(ConditionalExecutionPolicy *policy,uint64_t units) {
    if (!policy) return false;
    if (policy->calls==UINT64_MAX) { policy->refused=true;return false; }
    ++policy->calls;
    if (units>policy->remaining) { policy->refused=true;return false; }
    policy->remaining-=units;return true;
}

/* Refused input and cached reads cannot allocate or retain memory. */
static bool conditional_memory_unchanged(const XrXirCompileContext *context,
    XrCompileResourceStats before) {
    XrCompileResourceStats after=rp_stats(context);
    return after.held_bytes==before.held_bytes && after.allocated_bytes==before.allocated_bytes &&
        after.allocation_count==before.allocation_count;
}

static bool conditional_retry_reported[104];
static uint64_t conditional_retry_counts[104],conditional_retry_oom_counts[104];
static size_t conditional_retry_sites[104];
static bool conditional_retry_exact[104],conditional_retry_short[104];

/* The complete successful operation supplies its held peak. Turnover and
 * allocation counts are reported statistics, never admission allowances. */
static void conditional_retry_operation(const XrXirCompileContext *context,
    const ConditionalRetryTrial *trial) {
    CHECK(trial->which<104 && context->resources==trial->resources);
    XrCompileResourceStats consumed=rp_stats(context),initial=trial->initial,single=trial->single;
    CHECK(consumed.held_bytes==initial.held_bytes && single.held_bytes==initial.held_bytes);
    CHECK(single.allocated_bytes>=initial.allocated_bytes && single.allocation_count>=initial.allocation_count);
    rp_balanced(trial->retained);
    XrCompileResourceLimits limits=context->resources->limits;
    CHECK(limits.memory_mode==XR_COMPILE_MEMORY_FINITE && limits.profile_allocations);
    uint64_t bytes=single.allocated_bytes-initial.allocated_bytes;
    uint64_t allocations=single.allocation_count-initial.allocation_count;
    bool fits=single.peak_bytes<=limits.memory_limit;
    if (trial->first==XR_XIR_OUT_OF_MEMORY) {
        CHECK(rp_injected && trial->ordinal<trial->sites && fits);
        ++conditional_retry_oom_counts[trial->which];
    } else if (trial->first==XR_XIR_MEMORY_LIMIT) {
        CHECK(!fits && single.peak_bytes-limits.memory_limit==1);
        conditional_retry_short[trial->which]=true;
    } else {
        CHECK(trial->first==XR_XIR_OK && fits && limits.memory_limit==single.peak_bytes);
        conditional_retry_exact[trial->which]=true;
    }
    if (trial->sites>conditional_retry_sites[trial->which]) conditional_retry_sites[trial->which]=trial->sites;
    size_t attempts=rp_attempts;rp_fail_at=SIZE_MAX;
    XrXirStatus status=conditional_retry_case(context,trial->which);
    XrCompileResourceStats retried=rp_stats(context);
    CHECK(context->resources==trial->resources && retried.held_bytes==initial.held_bytes &&
        retried.allocated_bytes>=consumed.allocated_bytes &&
        retried.allocation_count>=consumed.allocation_count && rp_attempts>=attempts);
    rp_balanced(trial->retained);
    if (!fits && !conditional_retry_reported[trial->which]) {
        fprintf(stderr,"CONDITIONAL_RETRY_MEMORY case=%u ordinal=%zu first=%u result=%u "
            "required_peak=%llu memory_limit=%llu held=%llu allocated=%llu/%llu\n",
            trial->which,trial->ordinal,(uint32_t)trial->first,(uint32_t)status,
            (unsigned long long)single.peak_bytes,(unsigned long long)limits.memory_limit,
            (unsigned long long)retried.held_bytes,(unsigned long long)consumed.allocated_bytes,
            (unsigned long long)retried.allocated_bytes);
        conditional_retry_reported[trial->which]=true;
    }
    if (status!=(fits?XR_XIR_OK:XR_XIR_MEMORY_LIMIT)) fprintf(stderr,
        "CONDITIONAL_RETRY_FIRST case=%u ordinal=%zu first=%u result=%u fits=%u\n",
        trial->which,trial->ordinal,(uint32_t)trial->first,(uint32_t)status,(uint32_t)fits);
    CHECK(status==(fits?XR_XIR_OK:XR_XIR_MEMORY_LIMIT));
    if (fits) {
        CHECK(retried.allocated_bytes-consumed.allocated_bytes==bytes &&
            retried.allocation_count-consumed.allocation_count==allocations);
        CHECK(rp_attempts-attempts==trial->sites);
    }
    ++conditional_retry_counts[trial->which];
}

static void conditional_retry_coverage(uint32_t which) {
    CHECK(which<104 && conditional_retry_exact[which] && conditional_retry_short[which]);
    CHECK(conditional_retry_oom_counts[which]>=conditional_retry_sites[which] &&
        conditional_retry_counts[which]>=(uint64_t)conditional_retry_sites[which]+2);
}

/* Every case runs once before any retry. This short observer cannot
 * replace the original all-ordinal and resource-boundary registrations. */
static void conditional_retry_census(void) {
    for (uint32_t which=0;which<100;++which) {
        RootParameterMark physical=rp_mark();rp_fail_at=SIZE_MAX;rp_injected=false;
        XrXirCompileContext context=rp_owner(rp_caps());XrCompileResourceStats initial=rp_stats(&context);
        RootParameterMark retained=rp_mark();size_t attempts=rp_attempts;
        XrXirStatus status=conditional_retry_case(&context,which);XrCompileResourceStats single=rp_stats(&context);
        fprintf(stderr,"CONDITIONAL_RETRY_CENSUS case=%u status=%u sites=%zu "
            "initial_allocations=%llu allocations=%llu initial_bytes=%llu bytes=%llu "
            "held=%llu peak=%llu\n",which,(uint32_t)status,rp_attempts-attempts,
            (unsigned long long)initial.allocation_count,(unsigned long long)single.allocation_count,
            (unsigned long long)initial.allocated_bytes,(unsigned long long)single.allocated_bytes,
            (unsigned long long)single.held_bytes,(unsigned long long)single.peak_bytes);
        CHECK(status==XR_XIR_OK && single.held_bytes==initial.held_bytes);rp_balanced(retained);
        rp_owner_free(&context,initial.held_bytes);rp_balanced(physical);
    }
}

static void conditional_retry_complete(void) {
    for (uint32_t which=0;which<100;++which) conditional_retry_coverage(which);
}
#endif // XIR_CONDITIONAL_RETRY_CASES_H
