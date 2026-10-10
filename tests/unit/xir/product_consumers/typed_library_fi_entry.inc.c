/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * typed_library_fi_entry.inc.c - Owned status and allocation single-case entry
 *
 * KEY CONCEPT:
 *   The receiver consumes four once-generated materials without executing a writer or changing shared observers.
 */
#undef completed
#undef call_completed
#undef xr_xir_instance_poll_bounded
#undef xr_xir_instance_start
#undef xr_xir_instance_free
static TypedFiOwned typed_fi_owned(const ReceiverRun *run) {
    TypedFiOwned s = {0};
    s.pointers[0] = run->context ? run->context->resources : NULL;
    s.pointers[1] = run->source_checked; s.pointers[2] = run->closed; s.pointers[3] = run->lowered;
    s.pointers[4] = run->program; s.pointers[5] = run->instances[0]; s.pointers[6] = run->instances[1];
    s.pointers[7] = run->input_packet.bytes; s.pointers[8] = run->expected_closed.bytes;
    s.pointers[9] = run->transient_bytes; s.pointers[10] = run->pending_native_owner;
    s.pointers[11] = receiver_native_owner;
    ReceiverNativeOwner *owner = run->pending_native_owner;
    if (owner) {
        s.pointers[12] = owner->lowered; s.pointers[13] = owner->bindings;
        s.pointers[14] = owner->vm_entries; s.pointers[15] = owner->entries;
        s.pointers[16] = owner->native_resumes; s.pointers[17] = owner->vm_resumes;
    }
    s.packet_lengths[0] = run->input_packet.length; s.packet_lengths[1] = run->expected_closed.length;
    s.packet_lengths[2] = run->transient_size;
    memcpy(s.value_bytes, &run->value, sizeof(run->value));
    if (run->input_packet.bytes && run->input_packet.length)
        xr_sha256(run->input_packet.bytes, run->input_packet.length, s.input_digest);
    return s;
}
static void typed_fi_select_instance(ReceiverRun *run, XrXirInstance *instance) {
    typed_fi.active_slot = 2;
    for (unsigned i = 0; i < 2; ++i) if (run->instances[i] == instance && instance) typed_fi.active_slot = i;
    CHECK(typed_fi.active_slot < 2); /* Owned slot checked before the public call, not after free. */
}
static XrXirCallStatus typed_fi_start(ReceiverRun *run, XrXirInstance *instance, uint32_t entry,
    const XrXirValue *arguments, uint32_t count) {
    typed_fi_select_instance(run, instance);
    return xr_xir_instance_start(instance, entry, arguments, count);
}
static XrXirInstanceResult typed_fi_poll(ReceiverRun *run, XrXirInstance *instance, uint64_t quantum) {
    typed_fi_select_instance(run, instance);
    typed_fi_begin(run, "Instance-poll", TFI_CALL);
    XrXirInstanceResult result = xr_xir_instance_poll_bounded(instance, quantum);
    bool ordinary = result.outcome.status == XR_XIR_CALL_READY || result.outcome.status == XR_XIR_CALL_RETURNED ||
        result.outcome.status == XR_XIR_CALL_SUSPENDED;
    CHECK(xr_xir_call_result_valid(&result.outcome));
    typed_fi_end(run, (int)result.outcome.status, ordinary ? (int)result.outcome.status : XR_XIR_CALL_READY);
    return result; /* Borrowed result; never drop it. Owned result is taken by the original oracle. */
}
static XrXirCallStatus typed_fi_instance_free(ReceiverRun *run, XrXirInstance *instance) {
    typed_fi_select_instance(run, instance);
    typed_fi_begin(run, "Instance-free", TFI_CALL);
    XrXirCallStatus status = xr_xir_instance_free(instance);
    if (status != XR_XIR_CALL_BUSY)
        for (unsigned i = 0; i < 2; ++i) if (run->instances[i] == instance) run->instances[i] = NULL;
    typed_fi_end(run, (int)status, XR_XIR_CALL_READY);
    return status;
}
static void typed_fi_release_receiver(ReceiverRun *run) {
    xr_xir_value_drop(&run->value);
    for (unsigned i = 0; i < 2; ++i) {
        if (!run->instances[i]) continue;
        typed_fi.cleanup_called[i] = true;
        XrXirCallStatus status = xr_xir_instance_free(run->instances[i]);
        typed_fi.cleanup_status[i] = (int)status;
        if (status == XR_XIR_CALL_BUSY) { receiver_cleanup_failed = true; continue; }
        run->instances[i] = NULL; /* The public API consumed it even on failed cleanup. */
        bool inherited_oom = typed_fi.accepted_refusal && typed_fi.first_domain == TFI_CALL &&
            typed_fi.first_status == XR_XIR_CALL_OOM && typed_fi.first_slot == i && status == XR_XIR_CALL_OOM;
        typed_fi.cleanup_inherited_oom[i] = inherited_oom;
        if (status != XR_XIR_CALL_READY && !inherited_oom) receiver_cleanup_failed = true;
    }
    xr_xir_compile_program_drop(run->program); run->program = NULL;
    if (receiver_native_owner && !run->instances[0] && !run->instances[1]) {
        receiver_cleanup_failed = true; /* A missing real lease callback is never manufactured as PASS. */
        ReceiverNativeOwner *owner = receiver_native_owner; receiver_native_owner = NULL;
        dispose_receiver_native_owner(owner);
    }
    if (run->pending_native_owner) {
        ReceiverNativeOwner *owner = run->pending_native_owner; run->pending_native_owner = NULL;
        dispose_receiver_native_owner(owner);
    }
    free_poisoned_packet(&run->input_packet);
    poison(run->input_identity, sizeof(run->input_identity)); poison(run->proof_identity, sizeof(run->proof_identity));
    poison(run->packet_path, sizeof(run->packet_path)); poison(run->identity_path, sizeof(run->identity_path));
}
static bool typed_fi_owner(void) {
    ReceiverRun *run = &typed_fi.run;
    typed_fi.context.limits = xr_xir_compile_default_limits(); run->context = &typed_fi.context;
    typed_fi_begin(run, "owner-new", TFI_RESOURCE);
    XrCompileResourceStatus status = xr_compile_resources_new(&typed_fi.request.limits, &typed_fi.context.resources);
    typed_fi_end(run, (int)status, XR_COMPILE_RESOURCE_OK);
    if (status != XR_COMPILE_RESOURCE_OK) return false;
    CHECK(xr_compile_resources_stats(typed_fi.context.resources, &typed_fi.owner_baseline) == XR_COMPILE_RESOURCE_OK);
    return true;
}
static bool typed_fi_cumulative_same(TypedFiStock a, TypedFiStock b) {
    return a.compiler_sites == b.compiler_sites && a.runtime_sites == b.runtime_sites && a.valid == b.valid &&
        (!a.valid || (a.ledger.allocation_count == b.ledger.allocation_count &&
        a.ledger.allocated_bytes == b.ledger.allocated_bytes && a.ledger.peak_bytes == b.ledger.peak_bytes &&
        a.ledger.work == b.ledger.work));
}
static void typed_fi_cleanup(void) {
    ReceiverRun *run = &typed_fi.run;
    typed_fi.active = false; typed_fi.window = false;
    typed_fi.cleanup_before = typed_fi_stock(run);
    source_program_compile_fail_at = SIZE_MAX; runtime_fail_at = SIZE_MAX;
    receiver_cleanup_enabled = false; release_remaining(run);
    typed_fi.cleanup_after = typed_fi_stock(run);
    typed_fi.cleanup_uncharged = typed_fi_cumulative_same(typed_fi.cleanup_before, typed_fi.cleanup_after);
    typed_fi.cleanup_sites_unsupported = !typed_fi.cleanup_uncharged;
    typed_fi.final = typed_fi.cleanup_after.ledger;
    if (typed_fi.context.resources) {
        if (typed_fi.final.live_bytes != typed_fi.owner_baseline.live_bytes) receiver_cleanup_failed = true;
        xr_compile_resources_release(typed_fi.context.resources); typed_fi.context.resources = NULL;
    }
    typed_fi.physical_zero = !source_program_compile_live && !source_program_compile_bytes && !runtime_live &&
        !runtime_bytes && !runtime_owned && !runtime_owned_capacity;
    if (!typed_fi.physical_zero) receiver_cleanup_failed = true;
    if (source_program_compile_live == 0 && source_program_compile_bytes == 0) {
        xr_free(source_program_compile_allocations); source_program_compile_allocations = NULL;
        source_program_compile_capacity = 0;
    }
    typed_fi.physical_zero = typed_fi.physical_zero && !source_program_compile_allocations && !source_program_compile_capacity;
    printf("typed-library-fi cleanup called0=%u called1=%u status0=%d status1=%d inherit-oom0=%u inherit-oom1=%u "
        "uncharged=%u cleanup-sites-unsupported=%u "
        "cb=%zu ce=%zu rb=%zu re=%zu allocated-before=%llu allocated=%llu peak-before=%llu peak=%llu "
        "work-before=%llu work=%llu physical=%u lease-acquired=%u lease-releases=%u\n",
        (unsigned)typed_fi.cleanup_called[0], (unsigned)typed_fi.cleanup_called[1], typed_fi.cleanup_status[0],
        typed_fi.cleanup_status[1], (unsigned)typed_fi.cleanup_inherited_oom[0],
        (unsigned)typed_fi.cleanup_inherited_oom[1], (unsigned)typed_fi.cleanup_uncharged, (unsigned)typed_fi.cleanup_sites_unsupported,
        typed_fi.cleanup_before.compiler_sites, typed_fi.cleanup_after.compiler_sites,
        typed_fi.cleanup_before.runtime_sites, typed_fi.cleanup_after.runtime_sites,
        (unsigned long long)typed_fi.cleanup_before.ledger.allocated_bytes,
        (unsigned long long)typed_fi.cleanup_after.ledger.allocated_bytes,
        (unsigned long long)typed_fi.cleanup_before.ledger.peak_bytes, (unsigned long long)typed_fi.cleanup_after.ledger.peak_bytes,
        (unsigned long long)typed_fi.cleanup_before.ledger.work, (unsigned long long)typed_fi.cleanup_after.ledger.work,
        (unsigned)typed_fi.physical_zero, (unsigned)typed_fi.lease_acquired, receiver_native_releases);
}
static void typed_fi_reset(TypedFiRequest request, unsigned mode) {
    CHECK(!source_program_owner_count && !source_program_compile_live && !source_program_compile_bytes &&
        !source_program_compile_allocations && !source_program_compile_capacity && !runtime_live && !runtime_bytes &&
        !runtime_owned && !runtime_owned_capacity && !receiver_native_owner);
    source_program_compile_attempts = 0; source_program_compile_fail_at = SIZE_MAX;
    source_program_compile_injected = false; source_program_compile_peak_capacity = 0;
    source_program_compile_peak_live = 0; source_program_compile_peak_bytes = 0;
    runtime_attempts = 0; runtime_fail_at = SIZE_MAX;
    receiver_native_releases = 0; receiver_observer_failed = receiver_cleanup_failed = false;
    receiver_failure_line = 0; receiver_first_failure = (ReceiverFailureRecord){0};
    receiver_operation = "bootstrap"; typed_fi = (TypedFiCase){0};
    typed_fi.request = request; typed_fi.run.mode = mode; typed_fi.invariant = true;
    typed_fi.active_slot = typed_fi.first_slot = 2;
    typed_fi.cleanup_status[0] = typed_fi.cleanup_status[1] = -1;
}
static bool typed_fi_expected(void) {
    TypedFiRequest r = typed_fi.request;
    bool clean = typed_fi.physical_zero && typed_fi.cleanup_uncharged && !receiver_cleanup_failed &&
        !receiver_observer_failed && typed_fi.invariant && !receiver_native_owner &&
        receiver_native_releases == (typed_fi.lease_acquired ? 1u : 0u);
    if (r.kind == TFI_NORMAL || (r.kind == TFI_AXIS && !r.below))
        return clean && typed_fi.reached_end && !receiver_first_failure.domain && !typed_fi.hits;
    bool unique = r.kind == TFI_AXIS ? !typed_fi.hits : typed_fi.hits == 1;
    return clean && !typed_fi.reached_end && typed_fi.accepted_refusal && unique &&
        typed_fi.records == typed_fi.first_record + 1;
}
static void typed_fi_report_first_error(void) {
    ReceiverFailureRecord r = receiver_first_failure;
    if (!r.domain) return;
    printf("typed-library-fi first-error domain=%s operation=%s has-status=%u expected=%d actual=%d line=%d\n",
        r.domain, r.operation ? r.operation : "NONE", (unsigned)r.has_status, r.expected, r.actual, r.line);
}
static void typed_fi_summary(bool pass) {
    typed_fi_report_first_error();
    TypedFiRequest r = typed_fi.request;
    printf("typed-library-fi summary result=%s kind=%u mode=%u site=%zu total=%zu N=%zu R=%zu records=%zu hits=%zu "
        "first-record=%zu first-stage=%s domain=%u status=%d expected=%d first-slot=%u output-preserved=%u rollback=%u "
        "reached-end=%u downstream=%s allocated=%llu peak=%llu work=%llu physical=%u axes=%u below=%u "
        "cap-allocated=%llu cap-live=%llu cap-work=%llu "
        "original-writer-FI=OPEN stable-lifecycle=OPEN full-safety=OPEN Main=OPEN\n", pass ? "PASS" : "FAIL",
        r.kind, typed_fi.run.mode, r.site, r.total, source_program_compile_attempts, runtime_attempts,
        typed_fi.records, typed_fi.hits, typed_fi.first_record, typed_fi.first_stage ? typed_fi.first_stage : "NONE",
        typed_fi.first_domain, typed_fi.first_status, typed_fi.first_expected, typed_fi.first_slot, (unsigned)typed_fi.first_output_preserved,
        (unsigned)typed_fi.first_rollback, (unsigned)typed_fi.reached_end,
        typed_fi.accepted_refusal ? "NOT_ENTERED" : "NORMAL_OR_UNEXPECTED_FAILURE",
        (unsigned long long)typed_fi.final.allocated_bytes, (unsigned long long)typed_fi.final.peak_bytes,
        (unsigned long long)typed_fi.final.work, (unsigned)typed_fi.physical_zero, r.axis, (unsigned)r.below,
        (unsigned long long)r.limits.allocated_bytes, (unsigned long long)r.limits.live_bytes,
        (unsigned long long)r.limits.work);
}
static bool typed_fi_run(char **argv, TypedFiRequest request, unsigned mode) {
    typed_fi_reset(request, mode);
    source_program_compile_fail_at = request.kind == TFI_COMPILER ? request.site : SIZE_MAX;
    runtime_fail_at = request.kind == TFI_RUNTIME ? request.site : SIZE_MAX;
    if (setjmp(receiver_failure) == 0) {
        receiver_cleanup_enabled = true; typed_fi.active = true;
        if (typed_fi_owner() && admit_packet(&typed_fi.run, argv)) {
            run_receiver_instances(&typed_fi.run, argv[4]); typed_fi.reached_end = true;
        }
    }
    typed_fi_cleanup(); bool pass = typed_fi_expected(); typed_fi_summary(pass);
    return pass;
}
static bool typed_fi_number(const char *text, size_t *out) {
    if (!text || !*text) return false;
    size_t value = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || value > (SIZE_MAX - (size_t)(*p - '0')) / 10) return false;
        value = value * 10 + (size_t)(*p - '0');
    }
    if (value == SIZE_MAX) return false;
    *out = value; return true;
}
static int typed_fi_main(int argc, char **argv) {
    if (argc < 5) return 2;
    unsigned mode = !strcmp(argv[1], "1") ? 1u : !strcmp(argv[1], "2") ? 2u : !strcmp(argv[1], "3") ? 3u : 0u;
    if (!mode) return 2;
    TypedFiRequest r = {.limits = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)}};
    if (argc == 5) return typed_fi_run(argv, r, mode) ? 0 : 1;
    if (argc == 8 && (!strcmp(argv[5], "--compiler-fault") || !strcmp(argv[5], "--runtime-fault"))) {
        r.kind = !strcmp(argv[5], "--compiler-fault") ? TFI_COMPILER : TFI_RUNTIME;
        if (!typed_fi_number(argv[6], &r.site) || !typed_fi_number(argv[7], &r.total) || r.site >= r.total) return 2;
        return typed_fi_run(argv, r, mode) ? 0 : 1;
    }
    if (argc == 9 && !strcmp(argv[5], "--compiler-axis")) {
        r.kind = TFI_AXIS;
        r.axis = !strcmp(argv[6], "allocated") ? 0u : !strcmp(argv[6], "live") ? 1u : !strcmp(argv[6], "work") ? 2u : 3u;
        r.below = !strcmp(argv[7], "minus1"); size_t total = 0;
        if (r.axis > 2 || (!r.below && strcmp(argv[7], "exact")) || !typed_fi_number(argv[8], &total) || !total) return 2;
        uint64_t original = !r.axis ? r.limits.allocated_bytes : r.axis == 1 ? r.limits.live_bytes : r.limits.work;
        if ((uint64_t)total > original) return 2;
        r.total = total; uint64_t value = (uint64_t)total - (r.below ? 1u : 0u);
        if (!r.axis) r.limits.allocated_bytes = value;
        else if (r.axis == 1) r.limits.live_bytes = value;
        else r.limits.work = value;
        return typed_fi_run(argv, r, mode) ? 0 : 1;
    }
    return 2; /* Full shard dispatch remains OPEN; do not label single ordinal as full coverage. */
}
