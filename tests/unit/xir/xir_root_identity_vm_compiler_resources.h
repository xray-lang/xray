/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_identity_vm_compiler_resources.h - Real compiler frontier fault gates
 *
 * KEY CONCEPT:
 *   Every transition and retry uses the original finite compiler ledger.
 *   Qualification receives independently frozen census fields from its caller.
 */
#ifndef XIR_ROOT_IDENTITY_VM_COMPILER_RESOURCES_H
#define XIR_ROOT_IDENTITY_VM_COMPILER_RESOURCES_H

enum {
    RB_OWNER, RB_CHECK, RB_WRITE, RB_READ, RB_SPECIALIZE, RB_REVERIFY, RB_LOWER,
    RB_BIND0, RB_SEAL = RB_BIND0 + RI_FUNCTIONS, RB_PHASES,
    RB_VARIANTS = 2, RB_FIELDS = RB_PHASES + 3
};
static const char *const rb_names[RB_PHASES] = {
    "owner", "check", "write", "read", "specialize", "reverify", "lower",
    "bind0", "bind1", "bind2", "bind3", "bind4", "bind5", "bind6", "seal"
};
typedef struct RbStamp {
    XrCompileResourceStats stats;
    size_t attempts, blocks, bytes;
} RbStamp;
typedef struct RbOwner {
    EffectsSourceOwner *source;
    XrCompileResources *identity;
    XrCompileResourceLimits caps;
    XrXirCompileLimits structural;
    size_t blocks, bytes;
} RbOwner;
typedef struct RbPhase {
    RbStamp before, after;
    XrXirDiagnostic diagnostic;
    unsigned status;
} RbPhase;
typedef struct RbTrace {
    RbPhase phases[RB_PHASES];
    uint32_t seen, failed;
    unsigned variant;
    size_t point;
} RbTrace;
typedef struct RbExpected {
    uint64_t allocated, live, work;
    size_t sites[RB_PHASES];
} RbExpected;
static const XrCompileResourceLimits rb_defaults = {
    UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)
};
static RbExpected rb_expected[RB_VARIANTS];

static RbStamp rb_stamp(const RbOwner *owner) {
    RbStamp stamp = {{0}, effects_compile_attempts, effects_compile_live, effects_compile_bytes};
    if (owner && owner->source) {
        CHECK(owner->identity == owner->source->context.resources);
        CHECK(!memcmp(&owner->structural, &owner->source->context.limits, sizeof(owner->structural)));
        CHECK(!memcmp(&owner->caps, &owner->identity->limits, sizeof(owner->caps)));
        CHECK(xr_compile_resources_stats(owner->identity, &stamp.stats) == XR_COMPILE_RESOURCE_OK);
    }
    return stamp;
}
static void rb_log_stamp(const char *tag, const RbOwner *owner, const RbTrace *trace, RbStamp stamp) {
    printf("RB_LEDGER tag=%s variant=%u point=%zu owner=%p cap_alloc=%llu cap_live=%llu cap_work=%llu "
        "attempts=%zu allocations=%llu allocated=%llu live=%llu peak=%llu work=%llu "
        "physical_blocks=%zu physical_bytes=%zu\n", tag, trace->variant, trace->point,
        owner ? (void *)owner->identity : NULL,
        (unsigned long long)(owner ? owner->caps.allocated_bytes : 0),
        (unsigned long long)(owner ? owner->caps.live_bytes : 0),
        (unsigned long long)(owner ? owner->caps.work : 0), stamp.attempts,
        (unsigned long long)stamp.stats.allocation_count, (unsigned long long)stamp.stats.allocated_bytes,
        (unsigned long long)stamp.stats.live_bytes, (unsigned long long)stamp.stats.peak_bytes,
        (unsigned long long)stamp.stats.work, stamp.blocks, stamp.bytes);
}
static void rb_end(RbTrace *trace, unsigned phase, const RbOwner *owner,
    RbStamp before, unsigned status, XrXirDiagnostic diagnostic) {
    CHECK(phase < RB_PHASES);
    RbPhase *p = &trace->phases[phase];
    *p = (RbPhase){before, rb_stamp(owner), diagnostic, status};
    trace->seen |= UINT32_C(1) << phase;
    if (status) trace->failed = phase;
    printf("RB_STAGE variant=%u point=%zu phase=%s index=%u first=%zu end=%zu sites=%zu status=%u "
        "diag_provided=%u diag_status=%u function=%u block=%u instruction=%u reason=%u\n",
        trace->variant, trace->point, rb_names[phase], phase, before.attempts, p->after.attempts,
        p->after.attempts - before.attempts, status,
        (unsigned)(phase > RB_OWNER && phase < RB_BIND0), (unsigned)diagnostic.status,
        diagnostic.function, diagnostic.block, diagnostic.instruction, (unsigned)diagnostic.reason);
}
static XrCompileResourceStatus rb_new_owner(const XrCompileResourceLimits *caps,
    RbOwner *owner, RbTrace *trace) {
    CHECK(!effects_source_owner_count && !effects_compile_live && !effects_compile_bytes);
    CHECK(!owner->source && !owner->identity);
    EffectsSourceOwner *slot = &effects_source_owners[0];
    CHECK(!slot->context.resources);
    RbStamp before = rb_stamp(NULL);
    XrCompileResourceStatus status = xr_compile_resources_new(caps, &slot->context.resources);
    if (status == XR_COMPILE_RESOURCE_OK) {
        slot->context.limits = xr_xir_compile_default_limits();
        CHECK(xr_compile_resources_stats(slot->context.resources, &slot->baseline) == XR_COMPILE_RESOURCE_OK);
        effects_source_owner_count = 1;
        *owner = (RbOwner){slot, slot->context.resources, *caps, slot->context.limits,
            effects_compile_live, effects_compile_bytes};
        CHECK(owner->blocks == 1 && owner->bytes == slot->baseline.live_bytes);
    } else CHECK(!slot->context.resources && !effects_source_owner_count);
    rb_end(trace, RB_OWNER, owner->source ? owner : NULL, before, (unsigned)status, (XrXirDiagnostic){0});
    return status;
}
static void rb_physical_zero(const char *tag) {
    CHECK(!ri_observed && !runtime_live && !runtime_bytes && !runtime_owned && !runtime_owned_capacity);
    effects_source_owners_free();
    CHECK(!effects_compile_live && !effects_compile_bytes && !effects_source_owner_count);
    printf("RB_PHYSICAL tag=%s compiler=0/0 runtime=0/0 registry=0\n", tag);
}
static RbStamp rb_baseline(const RbOwner *owner) {
    RbStamp stamp = rb_stamp(owner);
    CHECK(stamp.stats.live_bytes == owner->source->baseline.live_bytes);
    CHECK(stamp.blocks == owner->blocks && stamp.bytes == owner->bytes);
    return stamp;
}
static void rb_preserved_fees(RbStamp before, RbStamp after) {
    CHECK(before.stats.allocation_count == after.stats.allocation_count);
    CHECK(before.stats.allocated_bytes == after.stats.allocated_bytes);
    CHECK(before.stats.work == after.stats.work && before.stats.peak_bytes == after.stats.peak_bytes);
}
static void rb_no_business(const RiFixture *f, size_t runtime_before) {
    CHECK(runtime_attempts == runtime_before && !runtime_live && !runtime_bytes);
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        const RiWitness *w = &f->witnesses[i];
        const unsigned empty[RI_FUNCTIONS] = {0};
        CHECK(!w->instance && !w->peer && !w->root_call && !w->active_view && !w->returned_view);
        CHECK(!memcmp(w->calls, empty, sizeof(empty)) && !memcmp(w->releases, empty, sizeof(empty)));
        CHECK(!w->function.type && !w->function.reserved && !w->function.payload);
        CHECK(!w->output_count && !w->lifecycle_count && !w->saved_root_live);
    }
}
static XrXirStatus rb_bind(RiFixture *f, uint32_t function, XrXirCallEntry *entry) {
    memset(&f->bindings[function], 0xa5, sizeof(f->bindings[function]));
    memset(&f->original[function], 0xa5, sizeof(f->original[function]));
    unsigned char saved_binding[sizeof(XrXirVmBinding)], saved_entry[sizeof(XrXirCallEntry)];
    memcpy(saved_binding, &f->bindings[function], sizeof(saved_binding));
    memcpy(saved_entry, &f->original[function], sizeof(saved_entry));
    XrXirStatus status = xr_xir_compile_vm_bind(f->lowered, function,
        &f->bindings[function], &f->original[function]);
    if (status != XR_XIR_OK) {
        CHECK(!memcmp(saved_binding, &f->bindings[function], sizeof(saved_binding)));
        CHECK(!memcmp(saved_entry, &f->original[function], sizeof(saved_entry)));
        return status;
    }
    *entry = f->original[function];
    entry->resume = ri_observe_resume; entry->release = ri_observe_release;
    return XR_XIR_OK;
}
static XrXirStatus rb_seal(const RbOwner *owner, RiFixture *f, const XrXirCallEntry *entries) {
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    const XrXirModule *module = xr_xir_compile_artifact_module(f->lowered);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, target, entries, RI_FUNCTIONS,
        module->declarations, {f, ri_code_drop}, module->types, xr_xir_compile_program_proof(f->lowered)};
    return xr_xir_compile_program_seal(&owner->source->context, &spec, &f->program);
}
static XrXirStatus rb_pipeline(const RbOwner *owner, RiFixture *f, RbTrace *trace) {
    CHECK(!ri_observed && owner->source && owner->identity);
    *f = (RiFixture){0}; ri_observed = f;
    RiGraph graph; ri_graph(&graph, trace->variant != 0);
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL;
    XrXirCheckedPacket packet = {0};
    XrXirCallEntry entries[RI_FUNCTIONS] = {0};
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    const XrXirCompileContext *context = &owner->source->context;
    XrXirStatus status = XR_XIR_OK;
    size_t runtime_before = runtime_attempts;
    trace->failed = UINT32_MAX;
    for (unsigned phase = RB_CHECK; phase < RB_PHASES; ++phase) {
        RbStamp before = rb_stamp(owner);
        XrXirDiagnostic diagnostic = {XR_XIR_OK, UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
        switch (phase) {
            case RB_CHECK: status = xr_xir_compile_check(context, &graph.module, &checked, &diagnostic); break;
            case RB_WRITE: status = xr_xir_compile_checked_write(checked, &packet, &diagnostic); break;
            case RB_READ: status = xr_xir_compile_checked_read(context, packet.bytes, packet.length, &decoded, &diagnostic); break;
            case RB_SPECIALIZE: status = xr_xir_compile_specialize(decoded, &closed, &diagnostic); break;
            case RB_REVERIFY: status = xr_xir_compile_artifact_verify(closed, &diagnostic); break;
            case RB_LOWER: status = xr_xir_compile_lower(closed, &target, &f->lowered, &diagnostic); break;
            case RB_SEAL: status = rb_seal(owner, f, entries); break;
            default: CHECK(phase >= RB_BIND0 && phase < RB_SEAL);
                status = rb_bind(f, phase - RB_BIND0, &entries[phase - RB_BIND0]); break;
        }
        rb_end(trace, phase, owner, before, (unsigned)status, diagnostic);
        if (phase < RB_BIND0) CHECK(diagnostic.status == status);
        rb_no_business(f, runtime_before);
        if (status != XR_XIR_OK) break;
        if (phase == RB_WRITE) {
            CHECK(checked && packet.bytes && packet.length);
            xr_xir_compile_artifact_free(checked); checked = NULL; memset(&graph, 0xa5, sizeof(graph));
        } else if (phase == RB_READ) {
            CHECK(decoded); memset(packet.bytes, 0xa5, packet.length); xr_xir_compile_checked_packet_free(&packet);
        } else if (phase == RB_SPECIALIZE) {
            CHECK(closed); xr_xir_compile_artifact_free(decoded); decoded = NULL;
        } else if (phase == RB_LOWER) {
            CHECK(f->lowered); xr_xir_compile_artifact_free(closed); closed = NULL;
        }
    }
    xr_xir_compile_artifact_free(checked); xr_xir_compile_artifact_free(decoded);
    xr_xir_compile_artifact_free(closed); xr_xir_compile_checked_packet_free(&packet);
    if (status != XR_XIR_OK) {
        CHECK(!f->program && !f->code_releases);
        xr_xir_compile_artifact_free(f->lowered); f->lowered = NULL;
        memset(f->bindings, 0, sizeof(f->bindings)); memset(f->original, 0, sizeof(f->original));
        ri_observed = NULL;
    } else CHECK(f->program && f->lowered && !f->code_releases && trace->failed == UINT32_MAX);
    rb_no_business(f, runtime_before);
    return status;
}
static void rb_program_release(const RbOwner *owner, RiFixture *f) {
    CHECK(f->program && f->lowered && !f->code_releases && ri_observed == f);
    xr_xir_compile_program_drop(f->program); f->program = NULL;
    CHECK(f->code_releases == 1 && !f->lowered); ri_observed = NULL;
    (void)rb_baseline(owner);
}
static void rb_normal_body(RiFixture *f) {
    XrXirInstanceResult suspended[RI_INSTANCES];
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        ri_start_root(&f->witnesses[i]); suspended[i] = ri_suspended(&f->witnesses[i]);
    }
    ri_cross_gate(f);
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        RiWitness *w = &f->witnesses[i];
        CHECK(xr_xir_instance_resume(w->instance, suspended[i].epoch, suspended[i].outcome.wake) == XR_XIR_CALL_READY);
        ri_returned(w); CHECK(ri_counter(w) == 11); ri_output_trace(w, 1);
        CHECK(w->root_cleanup_slot == 11 && w->child_local == 1);
        CHECK(w->releases[RI_ROOT] == 1 && w->releases[RI_CHILD] == 1 && w->releases[RI_READ] == 1);
        CHECK(w->releases[RI_ROOT_EXIT] == 1 && w->releases[RI_CHILD_EXIT] == 1);
        printf("RB_VM variant=0 instance=%u returned=42 output=6,42,5 counter=11\n", i);
    }
    ri_finish(f, false);
}
static void rb_failed_body(RiFixture *f) {
    XrXirCallResult escaped[RI_INSTANCES] = {0};
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        RiWitness *w = &f->witnesses[i]; ri_start_root(w);
        XrXirInstanceResult result = ri_poll(w);
        CHECK(result.outcome.status == XR_XIR_CALL_MATCH_FAILURE && result.outcome.panic.detail.code == 442);
        CHECK(xr_xir_instance_state(w->instance) == XR_XIR_INSTANCE_FAILED);
        CHECK(!w->calls[RI_ROOT] && !w->calls[RI_CHILD] && !w->output_count && !w->function.type);
        unsigned before[RI_FUNCTIONS]; memcpy(before, w->calls, sizeof(before));
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            CHECK(xr_xir_instance_start(w->instance, RI_ROOT, NULL, 0) == XR_XIR_CALL_MATCH_FAILURE);
            XrXirInstanceResult repeated = ri_poll(w);
            CHECK(repeated.outcome.status == XR_XIR_CALL_MATCH_FAILURE && repeated.epoch == result.epoch);
            CHECK(!memcmp(&repeated.outcome.panic.detail, &result.outcome.panic.detail, sizeof(result.outcome.panic.detail)));
            XrXirValue occupied = {XR_XIR_I64, 0, 909}, saved = occupied;
            CHECK(xr_xir_instance_take_result(w->instance, &occupied) == XR_XIR_CALL_BAD_STATE);
            CHECK(!memcmp(&occupied, &saved, sizeof(occupied)));
        }
        CHECK(!memcmp(before, w->calls, sizeof(before)));
        CHECK(xr_xir_instance_copy_failure(w->instance, &escaped[i]) == XR_XIR_CALL_MATCH_FAILURE);
        CHECK(escaped[i].status == XR_XIR_CALL_MATCH_FAILURE && !escaped[i].wake &&
            escaped[i].panic.detail.code == 442 && !escaped[i].panic.detail.reserved &&
            !escaped[i].panic.detail.index && !escaped[i].panic.detail.length);
        printf("RB_VM variant=1 instance=%u panic=442 sticky=1 root_calls=0 child_calls=0 epoch=%llu\n",
            i, (unsigned long long)result.epoch);
    }
    ri_finish(f, true);
    for (unsigned i = 0; i < RI_INSTANCES; ++i) {
        CHECK(escaped[i].status == XR_XIR_CALL_MATCH_FAILURE && escaped[i].panic.detail.code == 442);
        xr_xir_call_result_drop(&escaped[i]);
    }
}
static void rb_execute(const RbOwner *owner, RiFixture *f, const RbTrace *trace) {
    CHECK(effects_compile_fail_at == SIZE_MAX && runtime_fail_at == SIZE_MAX);
    ri_instances(f);
    if (trace->variant) rb_failed_body(f); else rb_normal_body(f);
    (void)rb_baseline(owner);
    rb_physical_zero(trace->variant ? "actual_retry_vm_sticky442" : "actual_retry_vm_42");
}
static size_t rb_total(const RbExpected *expected) {
    size_t total = 0;
    for (unsigned i = 0; i < RB_PHASES; ++i) {
        CHECK(expected->sites[i] <= SIZE_MAX - total); total += expected->sites[i];
    }
    CHECK(expected->sites[RB_OWNER] == 1 && total > 1);
    return total;
}
static void rb_counts(const RbTrace *trace, const RbExpected *expected, bool owner_included) {
    unsigned first = owner_included ? RB_OWNER : RB_CHECK;
    const uint32_t mask = ((UINT32_C(1) << RB_PHASES) - 1) & ~((UINT32_C(1) << first) - 1);
    CHECK(trace->seen == mask && trace->failed == UINT32_MAX);
    for (unsigned i = first; i < RB_PHASES; ++i) {
        const RbPhase *p = &trace->phases[i];
        CHECK(!p->status && p->after.attempts >= p->before.attempts);
        CHECK(p->after.attempts - p->before.attempts == expected->sites[i]);
    }
}
static void rb_exact_stats(RbStamp stamp, const RbExpected *expected) {
    CHECK(stamp.stats.allocated_bytes == expected->allocated);
    CHECK(stamp.stats.peak_bytes == expected->live && stamp.stats.work == expected->work);
    CHECK(stamp.stats.allocation_count == rb_total(expected));
}
static void rb_success_census(unsigned variant, bool frozen) {
    CHECK(effects_compile_fail_at == SIZE_MAX && runtime_fail_at == SIZE_MAX);
    RbTrace trace = {.failed = UINT32_MAX, .variant = variant, .point = SIZE_MAX};
    RbOwner owner = {0}; RiFixture f;
    CHECK(rb_new_owner(&rb_defaults, &owner, &trace) == XR_COMPILE_RESOURCE_OK);
    CHECK(rb_pipeline(&owner, &f, &trace) == XR_XIR_OK);
    RbStamp sealed = rb_stamp(&owner); size_t total = 0;
    printf("RB_CENSUS variant=%u allocated=%llu live=%llu work=%llu sites=", variant,
        (unsigned long long)sealed.stats.allocated_bytes, (unsigned long long)sealed.stats.peak_bytes,
        (unsigned long long)sealed.stats.work);
    for (unsigned i = 0; i < RB_PHASES; ++i) {
        size_t sites = trace.phases[i].after.attempts - trace.phases[i].before.attempts;
        CHECK(sites <= SIZE_MAX - total); total += sites;
        printf("%s%zu", i ? "," : "", sites);
    }
    printf(" total=%zu generic_expansion=NOT_COVERED\n", total);
    CHECK(total == sealed.stats.allocation_count && trace.seen == ((UINT32_C(1) << RB_PHASES) - 1));
    if (frozen) { rb_counts(&trace, &rb_expected[variant], true); rb_exact_stats(sealed, &rb_expected[variant]); }
    rb_log_stamp("seal_census", &owner, &trace, sealed);
    rb_execute(&owner, &f, &trace);
}
static void rb_original_cases(void) {
    ri_identity_case(); rb_physical_zero("original_identity");
    ri_resume_case(); rb_physical_zero("original_resume");
    ri_closing_case(); rb_physical_zero("original_closing");
    ri_initialization_failure_case(); rb_physical_zero("original_sticky442");
}
static uint64_t rb_integer(const char *text) {
    CHECK(text && *text >= '0' && *text <= '9');
    char *end = NULL; errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    CHECK(!errno && end && !*end && (unsigned long long)(uint64_t)value == value);
    return (uint64_t)value;
}
static void rb_arguments(int argc, char **argv) {
    CHECK(argc == 2 + RB_VARIANTS * RB_FIELDS);
    for (unsigned variant = 0; variant < RB_VARIANTS; ++variant) {
        unsigned begin = 2 + variant * RB_FIELDS;
        RbExpected *e = &rb_expected[variant];
        e->allocated = rb_integer(argv[begin]); e->live = rb_integer(argv[begin + 1]);
        e->work = rb_integer(argv[begin + 2]); CHECK(e->allocated && e->live && e->work);
        CHECK(e->allocated <= rb_defaults.allocated_bytes && e->live <= rb_defaults.live_bytes && e->work <= rb_defaults.work);
        for (unsigned i = 0; i < RB_PHASES; ++i) {
            uint64_t sites = rb_integer(argv[begin + 3 + i]); CHECK((uint64_t)(size_t)sites == sites);
            e->sites[i] = (size_t)sites;
        }
        (void)rb_total(e);
    }
}
static unsigned rb_point_phase(const RbExpected *e, size_t point) {
    size_t start = 0;
    for (unsigned i = 0; i < RB_PHASES; ++i) {
        CHECK(e->sites[i] <= SIZE_MAX - start);
        if (point < start + e->sites[i]) return i;
        start += e->sites[i];
    }
    CHECK(false); return UINT32_MAX;
}
static void rb_retry_fees(const RbOwner *owner, RbStamp before, RbStamp after, const RbExpected *e) {
    CHECK(e->allocated >= owner->source->baseline.allocated_bytes && e->work >= owner->source->baseline.work);
    CHECK(after.stats.allocated_bytes - before.stats.allocated_bytes ==
        e->allocated - owner->source->baseline.allocated_bytes);
    CHECK(after.stats.work - before.stats.work == e->work - owner->source->baseline.work);
    CHECK(after.stats.allocation_count - before.stats.allocation_count == rb_total(e) - 1);
    CHECK(after.stats.peak_bytes == e->live);
}
static void rb_one_oom(unsigned variant, size_t point) {
    const RbExpected *e = &rb_expected[variant];
    unsigned expected_phase = rb_point_phase(e, point);
    RbTrace first = {.failed = UINT32_MAX, .variant = variant, .point = point};
    RbOwner owner = {0}; RiFixture f;
    size_t begin = effects_compile_attempts;
    CHECK(point < SIZE_MAX - begin); effects_compile_fail_at = begin + point; effects_compile_injected = false;
    XrCompileResourceStatus created = rb_new_owner(&rb_defaults, &owner, &first);
    if (expected_phase == RB_OWNER) {
        CHECK(created == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !owner.source && !owner.identity);
        CHECK(effects_compile_injected && effects_compile_attempts == begin + point + 1);
        effects_compile_fail_at = SIZE_MAX; rb_physical_zero("owner_oom_no_owner");
        effects_compile_injected = false;
        RbTrace fresh = {.failed = UINT32_MAX, .variant = variant, .point = point};
        CHECK(rb_new_owner(&rb_defaults, &owner, &fresh) == XR_COMPILE_RESOURCE_OK);
        CHECK(rb_pipeline(&owner, &f, &fresh) == XR_XIR_OK);
        CHECK(!effects_compile_injected);
        rb_counts(&fresh, e, true); rb_exact_stats(rb_stamp(&owner), e);
        printf("RB_OOM variant=%u point=%zu denominator=%zu phase=owner layer=resource status=%u injected=1 same_owner=0\n",
            variant, point, rb_total(e), (unsigned)created);
        puts("RB_RETRY owner_case=fresh_owner same_owner=0");
        rb_execute(&owner, &f, &fresh); return;
    }
    CHECK(created == XR_COMPILE_RESOURCE_OK);
    RbStamp s0 = rb_stamp(&owner); rb_log_stamp("S0", &owner, &first, s0);
    XrXirStatus status = rb_pipeline(&owner, &f, &first);
    CHECK(status == XR_XIR_OUT_OF_MEMORY && first.failed == expected_phase);
    CHECK(effects_compile_injected && effects_compile_attempts == begin + point + 1);
    effects_compile_fail_at = SIZE_MAX;
    RbStamp s1 = rb_baseline(&owner); rb_log_stamp("S1", &owner, &first, s1);
    rb_preserved_fees(first.phases[first.failed].after, s1);
    CHECK(s1.stats.work > s0.stats.work && s1.stats.allocated_bytes >= s0.stats.allocated_bytes);
    CHECK(s1.stats.allocation_count - s0.stats.allocation_count == point - 1);
    RbTrace retry = {.failed = UINT32_MAX, .variant = variant, .point = point};
    effects_compile_injected = false;
    CHECK(rb_pipeline(&owner, &f, &retry) == XR_XIR_OK);
    CHECK(!effects_compile_injected);
    rb_counts(&retry, e, false);
    RbStamp s2 = rb_stamp(&owner); rb_log_stamp("S2", &owner, &retry, s2);
    rb_retry_fees(&owner, s1, s2, e);
    printf("RB_OOM variant=%u point=%zu denominator=%zu phase=%s layer=xir status=%u injected=1 same_owner=1\n",
        variant, point, rb_total(e), rb_names[expected_phase], (unsigned)status);
    rb_execute(&owner, &f, &retry);
}
static void rb_oom_range(unsigned first_phase, unsigned last_phase) {
    CHECK(first_phase <= last_phase && last_phase < RB_PHASES);
    for (unsigned variant = 0; variant < RB_VARIANTS; ++variant) {
        rb_success_census(variant, true);
        const RbExpected *e = &rb_expected[variant];
        size_t first = 0, count = 0;
        for (unsigned i = 0; i < first_phase; ++i) first += e->sites[i];
        for (unsigned i = first_phase; i <= last_phase; ++i) count += e->sites[i];
        size_t completed = 0;
        for (size_t i = 0; i < count; ++i) { rb_one_oom(variant, first + i); ++completed; }
        CHECK(completed == count);
        printf("RB_OOM_DONE variant=%u first_phase=%s last_phase=%s attempted=%zu expected=%zu\n",
            variant, rb_names[first_phase], rb_names[last_phase], completed, count);
    }
}
static void rb_budget_one(unsigned variant, unsigned axis, bool minus_one) {
    const RbExpected *e = &rb_expected[variant];
    XrCompileResourceLimits caps = rb_defaults;
    if (axis == 0) caps.allocated_bytes = e->allocated - (unsigned)minus_one;
    else if (axis == 1) caps.live_bytes = e->live - (unsigned)minus_one;
    else { CHECK(axis == 2); caps.work = e->work - (unsigned)minus_one; }
    RbTrace trace = {.failed = UINT32_MAX, .variant = variant, .point = axis};
    RbOwner owner = {0}; RiFixture f;
    CHECK(rb_new_owner(&caps, &owner, &trace) == XR_COMPILE_RESOURCE_OK);
    RbStamp s0 = rb_stamp(&owner); rb_log_stamp("budget_S0", &owner, &trace, s0);
    XrXirStatus status = rb_pipeline(&owner, &f, &trace);
    RbStamp end = rb_stamp(&owner); rb_log_stamp("budget_end", &owner, &trace, end);
    if (minus_one) {
        CHECK(status == XR_XIR_BUDGET && trace.failed < RB_PHASES && !f.program && !f.lowered && !f.code_releases);
        rb_preserved_fees(trace.phases[trace.failed].after, rb_baseline(&owner));
    } else {
        CHECK(status == XR_XIR_OK); rb_counts(&trace, e, true); rb_exact_stats(end, e);
        rb_program_release(&owner, &f); rb_preserved_fees(end, rb_baseline(&owner));
    }
    printf("RB_BUDGET variant=%u axis=%u minus1=%u status=%u failure_phase=%u "
        "compiler_frontier_only=1 vm_poll=NOT_RUN\n",
        variant, axis, (unsigned)minus_one, (unsigned)status, trace.failed);
    (void)rb_baseline(&owner); rb_physical_zero("compiler_exact_minus1");
}
static void rb_budget(void) {
    for (unsigned variant = 0; variant < RB_VARIANTS; ++variant) {
        rb_success_census(variant, true);
        for (unsigned axis = 0; axis < 3; ++axis) {
            rb_budget_one(variant, axis, false); rb_budget_one(variant, axis, true);
        }
    }
    puts("RB_BUDGET_DONE variants=2 controls=12 exact_status=OK minus1_status=BUDGET physical=0/0,0/0");
}
static void rb_samequota(void) {
    for (unsigned variant = 0; variant < RB_VARIANTS; ++variant) {
        const RbExpected *e = &rb_expected[variant]; rb_success_census(variant, true);
        CHECK(e->sites[RB_CHECK] && e->work > 1);
        XrCompileResourceLimits caps = rb_defaults; caps.work = e->work;
        RbTrace first = {.failed = UINT32_MAX, .variant = variant, .point = 1};
        RbOwner owner = {0}; RiFixture f;
        CHECK(rb_new_owner(&caps, &owner, &first) == XR_COMPILE_RESOURCE_OK);
        RbStamp s0 = rb_stamp(&owner); rb_log_stamp("samequota_S0", &owner, &first, s0);
        effects_compile_fail_at = effects_compile_attempts; effects_compile_injected = false;
        XrXirStatus status = rb_pipeline(&owner, &f, &first);
        CHECK(status == XR_XIR_OUT_OF_MEMORY && first.failed == RB_CHECK && effects_compile_injected);
        CHECK(effects_compile_attempts == s0.attempts + 1);
        effects_compile_fail_at = SIZE_MAX;
        RbStamp s1 = rb_baseline(&owner); rb_log_stamp("samequota_S1", &owner, &first, s1);
        rb_preserved_fees(first.phases[first.failed].after, s1);
        CHECK(s1.stats.work > s0.stats.work && s1.stats.allocated_bytes == s0.stats.allocated_bytes);
        CHECK(s1.stats.allocation_count == s0.stats.allocation_count);
        RbTrace retry = {.failed = UINT32_MAX, .variant = variant, .point = 1};
        effects_compile_injected = false;
        CHECK(rb_pipeline(&owner, &f, &retry) == XR_XIR_BUDGET);
        CHECK(retry.failed < RB_PHASES && !effects_compile_injected);
        RbStamp s2 = rb_baseline(&owner); rb_log_stamp("samequota_S2", &owner, &retry, s2);
        rb_preserved_fees(retry.phases[retry.failed].after, s2);
        CHECK(s2.stats.work >= s1.stats.work && s2.stats.allocated_bytes >= s1.stats.allocated_bytes);
        CHECK(!f.program && !f.lowered && !f.code_releases);
        printf("RB_SAMEQUOTA variant=%u OOM_then_BUDGET=1 same_owner=1 phase=%s work_limit=%llu\n",
            variant, rb_names[retry.failed], (unsigned long long)caps.work);
        rb_physical_zero("samequota_paid_failure");
    }
}
static void rb_qualification(const char *mode) {
    if (!strcmp(mode, "budget")) { rb_budget(); return; }
    if (!strcmp(mode, "samequota_retry")) { rb_samequota(); return; }
    if (!strcmp(mode, "owner_oom")) { rb_oom_range(RB_OWNER, RB_OWNER); return; }
    if (!strcmp(mode, "bind_oom")) { rb_oom_range(RB_BIND0, RB_SEAL - 1); return; }
    static const unsigned phases[] = {RB_CHECK, RB_WRITE, RB_READ, RB_SPECIALIZE, RB_REVERIFY, RB_LOWER, RB_SEAL};
    for (unsigned i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i) {
        char name[32];
        int length = snprintf(name, sizeof(name), "%s_oom", rb_names[phases[i]]);
        CHECK(length > 0 && (size_t)length < sizeof(name));
        if (!strcmp(mode, name)) { rb_oom_range(phases[i], phases[i]); return; }
    }
    CHECK(false);
}
#endif // XIR_ROOT_IDENTITY_VM_COMPILER_RESOURCES_H
