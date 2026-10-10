/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_library_fi_protocol.inc.c - Dedicated receiver allocation and status observations
 *
 * KEY CONCEPT:
 *   Each public-call window preserves exact domain status, owned outputs and cumulative resource observations.
 */
enum { TFI_RESOURCE, TFI_XIR, TFI_CALL, TFI_CLEANUP };
enum { TFI_NORMAL, TFI_COMPILER, TFI_RUNTIME, TFI_AXIS };
typedef struct TypedFiStock {
    size_t compiler_sites, runtime_sites, compiler_live, compiler_bytes, runtime_live, runtime_bytes;
    XrCompileResourceStats ledger;
    bool valid;
} TypedFiStock;
typedef struct TypedFiOwned {
    const void *pointers[18];
    size_t packet_lengths[3];
    uint8_t input_digest[32], value_bytes[sizeof(XrXirValue)];
} TypedFiOwned;
typedef struct TypedFiRequest {
    unsigned kind, axis;
    bool below;
    size_t site, total;
    XrCompileResourceLimits limits;
} TypedFiRequest;
typedef struct TypedFiCase {
    ReceiverRun run;
    XrXirCompileContext context;
    XrCompileResourceStats owner_baseline, final;
    TypedFiRequest request;
    TypedFiStock before, cleanup_before, cleanup_after;
    TypedFiOwned owned_before;
    const char *stage, *first_stage;
    size_t records, hits, first_record, completed_calls;
    unsigned domain, first_domain, active_slot, first_slot;
    int first_status, first_expected, cleanup_status[2];
    bool active, window, reached_end, accepted_refusal, invariant, cleanup_uncharged;
    bool cleanup_called[2], cleanup_inherited_oom[2];
    bool first_output_preserved, first_rollback, cleanup_sites_unsupported, physical_zero, lease_acquired;
} TypedFiCase;
static TypedFiCase typed_fi;
static bool completed(ReceiverRun *, XrXirStatus, const char *);
static bool call_completed(ReceiverRun *, XrXirCallStatus, XrXirCallStatus, const char *);
static TypedFiOwned typed_fi_owned(const ReceiverRun *);
static XrXirInstanceResult typed_fi_poll(ReceiverRun *, XrXirInstance *, uint64_t);
static XrXirCallStatus typed_fi_start(ReceiverRun *, XrXirInstance *, uint32_t, const XrXirValue *, uint32_t);
static XrXirCallStatus typed_fi_instance_free(ReceiverRun *, XrXirInstance *);
static void typed_fi_release_receiver(ReceiverRun *);
static int typed_fi_main(int, char **);
static TypedFiStock typed_fi_stock(const ReceiverRun *run) {
    TypedFiStock s = {0};
    s.compiler_sites = source_program_compile_attempts; s.runtime_sites = runtime_attempts;
    s.compiler_live = source_program_compile_live; s.compiler_bytes = source_program_compile_bytes;
    s.runtime_live = runtime_live; s.runtime_bytes = runtime_bytes;
    s.valid = run->context && run->context->resources &&
        xr_compile_resources_stats(run->context->resources, &s.ledger) == XR_COMPILE_RESOURCE_OK;
    return s;
}
static bool typed_fi_same_stock(TypedFiStock a, TypedFiStock b) {
    return a.compiler_live == b.compiler_live && a.compiler_bytes == b.compiler_bytes &&
        a.runtime_live == b.runtime_live && a.runtime_bytes == b.runtime_bytes &&
        a.valid == b.valid && (!a.valid || a.ledger.live_bytes == b.ledger.live_bytes);
}
static int typed_fi_oom(unsigned domain) {
    if (domain == TFI_RESOURCE) return XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    if (domain == TFI_XIR) return XR_XIR_OUT_OF_MEMORY;
    return domain == TFI_CALL ? XR_XIR_CALL_OOM : -1;
}
static int typed_fi_budget(unsigned domain) {
    if (domain == TFI_RESOURCE) return XR_COMPILE_RESOURCE_BUDGET;
    if (domain == TFI_XIR) return XR_XIR_BUDGET;
    return -1; /* Compiler axes never reinterpret a runtime LIMIT as compiler BUDGET. */
}
static void typed_fi_begin(ReceiverRun *run, const char *stage, unsigned domain) {
    if (!typed_fi.active) return;
    CHECK(!typed_fi.window && !typed_fi.accepted_refusal);
    run->operation = receiver_operation = stage;
    typed_fi.stage = stage; typed_fi.domain = domain; typed_fi.window = true;
    if (domain != TFI_CALL || !strcmp(stage, "Instance-new") || !strcmp(stage, "xr-xir-instance-config-init"))
        typed_fi.active_slot = 2;
    typed_fi.before = typed_fi_stock(run); typed_fi.owned_before = typed_fi_owned(run);
}
static void typed_fi_stage_row(TypedFiStock after, int status, int expected, bool hit, bool output, bool rollback) {
    TypedFiStock before = typed_fi.before;
    printf("typed-library-fi stage record=%zu name=%s domain=%u status=%d expected=%d hit=%u output-preserved=%u rollback=%u "
        "cb=%zu ce=%zu rb=%zu re=%zu count-before=%llu count-after=%llu allocated-before=%llu allocated=%llu "
        "live-before=%llu live=%llu peak-before=%llu peak=%llu work-before=%llu work=%llu valid-before=%u valid=%u "
        "compiler-stock=%zu/%zu runtime-stock=%zu/%zu\n",
        typed_fi.records, typed_fi.stage, typed_fi.domain, status, expected, (unsigned)hit,
        (unsigned)output, (unsigned)rollback, before.compiler_sites, after.compiler_sites,
        before.runtime_sites, after.runtime_sites, (unsigned long long)before.ledger.allocation_count,
        (unsigned long long)after.ledger.allocation_count, (unsigned long long)before.ledger.allocated_bytes,
        (unsigned long long)after.ledger.allocated_bytes, (unsigned long long)before.ledger.live_bytes,
        (unsigned long long)after.ledger.live_bytes, (unsigned long long)before.ledger.peak_bytes,
        (unsigned long long)after.ledger.peak_bytes, (unsigned long long)before.ledger.work,
        (unsigned long long)after.ledger.work, (unsigned)before.valid, (unsigned)after.valid,
        after.compiler_live, after.compiler_bytes, after.runtime_live, after.runtime_bytes);
}
static bool typed_fi_hit(TypedFiStock after) {
    TypedFiRequest r = typed_fi.request;
    if (r.kind == TFI_COMPILER)
        return typed_fi.before.compiler_sites <= r.site && r.site < after.compiler_sites && source_program_compile_injected;
    return r.kind == TFI_RUNTIME && typed_fi.before.runtime_sites <= r.site && r.site < after.runtime_sites;
}
static void typed_fi_reject(int status, int expected, bool accepted, bool output, bool rollback) {
    typed_fi.first_record = typed_fi.records - 1; typed_fi.first_stage = typed_fi.stage;
    typed_fi.first_domain = typed_fi.domain; typed_fi.first_status = status; typed_fi.first_expected = expected;
    typed_fi.first_output_preserved = output; typed_fi.first_rollback = rollback;
    typed_fi.first_slot = typed_fi.domain == TFI_CALL ? typed_fi.active_slot : 2u;
    typed_fi.accepted_refusal = accepted;
    record_failure(typed_fi.domain == TFI_RESOURCE ? "COMPILE_RESOURCE_STATUS" :
        typed_fi.domain == TFI_XIR ? "PRODUCT_XIR_STATUS" : "PRODUCT_CALL_STATUS",
        typed_fi.stage, true, expected, status, NULL);
    longjmp(receiver_failure, 1); /* Caller boundary only; never a runtime callback. */
}
static void typed_fi_end(ReceiverRun *run, int status, int normal_status) {
    if (!typed_fi.active) return;
    CHECK(typed_fi.window); typed_fi.window = false;
    TypedFiStock after = typed_fi_stock(run); TypedFiOwned owned = typed_fi_owned(run);
    if (run->program) typed_fi.lease_acquired = true;
    bool output = !memcmp(&owned, &typed_fi.owned_before, sizeof(owned));
    bool rollback = typed_fi_same_stock(typed_fi.before, after), hit = typed_fi_hit(after);
    int wanted = typed_fi.request.kind == TFI_NORMAL ? normal_status :
        typed_fi.request.kind == TFI_AXIS ? typed_fi_budget(typed_fi.domain) : typed_fi_oom(typed_fi.domain);
    bool refused = status == wanted && wanted >= 0;
    bool accepted = typed_fi.request.kind == TFI_AXIS ? typed_fi.request.below && refused && !hit : hit && refused;
    if (typed_fi.domain == TFI_RESOURCE || typed_fi.domain == TFI_XIR) accepted = accepted && output && rollback;
    if (typed_fi.domain == TFI_CALL && !strcmp(typed_fi.stage, "Instance-new")) accepted = accepted && output;
    if (hit) ++typed_fi.hits;
    if (after.compiler_sites < typed_fi.before.compiler_sites || after.runtime_sites < typed_fi.before.runtime_sites)
        typed_fi.invariant = false;
    typed_fi_stage_row(after, status, normal_status, hit, output, rollback); ++typed_fi.records;
    if (hit || status != normal_status) typed_fi_reject(status, wanted, accepted, output, rollback);
}
static bool typed_fi_xir_result(ReceiverRun *run, XrXirStatus actual, const char *stage) {
    typed_fi_end(run, (int)actual, XR_XIR_OK);
    return completed(run, actual, stage);
}
static bool typed_fi_call_result(ReceiverRun *run, XrXirCallStatus actual, XrXirCallStatus expected, const char *stage) {
    typed_fi_end(run, (int)actual, (int)expected);
    return call_completed(run, actual, expected, stage);
}
