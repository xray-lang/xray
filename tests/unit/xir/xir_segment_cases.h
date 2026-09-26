/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_segment_cases.h - Stable frames across expansion, suspension and reuse
 *
 * KEY CONCEPT:
 *   Real entry callbacks keep and check ancestor addresses and payloads.
 */
#ifndef XIR_SEGMENT_CASES_H
#define XIR_SEGMENT_CASES_H
typedef struct SegmentState { uint32_t phase; XrXirValue child; } SegmentState;
typedef struct SegmentWitness {
    void *states[64];
    const XrXirValue *arguments[64];
    uint32_t root, state_bytes, passes, mode, cleanups, last_cleanup;
} SegmentWitness;
static const XrXirType segment_type = XR_XIR_I64;
static void segment_check(XrXirCallView *view) {
    SegmentWitness *w = view->instance;
    uint32_t level = (uint32_t) view->arguments[0].payload;
    CHECK(level <= w->root && view->state == w->states[level] && view->arguments == w->arguments[level]);
    CHECK((uintptr_t) view->state % 16 == 0);
    for (uint32_t i = level; i <= w->root; ++i) {
        const unsigned char *bytes = w->states[i];
        CHECK(bytes && w->arguments[i]->payload == i);
        for (uint32_t b = sizeof(SegmentState); b < w->state_bytes; ++b) CHECK(bytes[b] == (unsigned char) (i + 1));
    }
#if defined(XR_XIR_FRAME_ASAN)
    CallFrame *frame = view->activation->top;
    CHECK(__asan_address_is_poisoned((unsigned char *) frame + frame->allocation_bytes - 1));
    CallSegment *segment = view->activation->segment;
    if (segment->used < segment->allocation_bytes)
        CHECK(__asan_address_is_poisoned((unsigned char *) segment + segment->used));
#endif
}
static XrXirAction segment_resume(XrXirCallView *view) {
    SegmentWitness *w = view->instance; SegmentState *state = view->state;
    uint32_t level = (uint32_t) view->arguments[0].payload;
    if (!state->phase) {
        for (uint32_t b = 0; b < w->state_bytes; ++b) CHECK(!((unsigned char *) state)[b]);
        w->states[level] = state; w->arguments[level] = view->arguments;
        memset((unsigned char *) state + sizeof(*state), (int) (level + 1), w->state_bytes - sizeof(*state));
        state->phase = 1;
        segment_check(view);
        if (!level) return (XrXirAction) {XR_XIR_ACTION_SUSPEND, 0, NULL, 0, {0}};
        state->child = (XrXirValue) {XR_XIR_I64, 0, level - 1};
        return (XrXirAction) {XR_XIR_ACTION_CALL, 0, &state->child, 1, {0}};
    }
    segment_check(view);
    if (level) {
#if defined(XR_XIR_FRAME_ASAN)
        CHECK(__asan_address_is_poisoned(w->states[level - 1]));
#endif
        if (view->inbox.status == XR_XIR_CALL_THROWN)
            return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, view->inbox.value};
        CHECK(view->inbox.status == XR_XIR_CALL_RETURNED && view->inbox.value.payload == level - 1);
        if (level == w->root && state->phase++ < w->passes) {
            w->last_cleanup = UINT32_MAX;
            return (XrXirAction) {XR_XIR_ACTION_CALL, 0, &state->child, 1, {0}};
        }
    }
    if (w->mode == 2) return (XrXirAction) {XR_XIR_ACTION_THROW, 0, NULL, 0, {XR_XIR_I64, 0, 71}};
    return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, {XR_XIR_I64, 0, level}};
}
static void segment_cleanup(XrXirCallView *view, XrXirCallStatus reason) {
    SegmentWitness *w = view->instance; SegmentState *state = view->state;
    uint32_t level = (uint32_t) view->arguments[0].payload;
    if (state->phase) segment_check(view);
    if (reason == XR_XIR_CALL_RETURNED || reason == XR_XIR_CALL_THROWN) {
        if (level) CHECK(w->last_cleanup + 1 == level);
    } else if (w->last_cleanup != UINT32_MAX) CHECK(w->last_cleanup + 1 == level);
    w->last_cleanup = level; ++w->cleanups;
}
static XrXirCallEntry segment_entry(uint32_t bytes) {
    return (XrXirCallEntry) {XR_XIR_CALL_ABI_VERSION, &segment_type, 1, XR_XIR_I64,
        bytes, segment_resume, segment_cleanup, NULL};
}
static size_t segment_attempt(uint32_t bytes, uint32_t mode, uint32_t passes) {
    SegmentWitness witness = {0}; witness.root = 48; witness.state_bytes = bytes;
    witness.mode = mode; witness.passes = passes; witness.last_cleanup = UINT32_MAX;
    XrXirCallEntry entry = segment_entry(bytes); XrXirCallAccounting accounting = {0};
    XrXirCallConfig config = {&entry, 1, &witness, 2 * 1024 * 1024, 100000, 64, &accounting, {0}};
    XrXirValue argument = {XR_XIR_I64, 0, 48}; XrXirCall *call = NULL;
    XrXirCallStatus status = xr_xir_call_new(&config, 0, &argument, 1, &call);
    uint32_t wakes = 0;
    if (status == XR_XIR_CALL_READY && mode != 3) {
        XrXirCallResult result = xr_xir_call_poll(call);
        while (result.status == XR_XIR_CALL_SUSPENDED) {
            CHECK(accounting.depth == 49 && ++wakes <= passes);
            if (mode == 1) {
                CHECK(xr_xir_call_cancel(call) == XR_XIR_CALL_CANCELLED);
                result = xr_xir_call_poll(call); break;
            }
            if (mode == 4) break;
            CHECK(xr_xir_call_resume(call, result.wake) == XR_XIR_CALL_READY);
            result = xr_xir_call_poll(call);
        }
        status = result.status;
        if (fail_at != SIZE_MAX) CHECK(status == XR_XIR_CALL_OOM);
        else if (mode == 1) CHECK(status == XR_XIR_CALL_CANCELLED && witness.cleanups == 49);
        else if (mode == 2) CHECK(status == XR_XIR_CALL_THROWN && result.value.payload == 71 && witness.cleanups == 49);
        else if (mode != 4) CHECK(status == XR_XIR_CALL_RETURNED && result.value.payload == 48 && witness.cleanups == 48 * passes + 1);
        if (status != XR_XIR_CALL_SUSPENDED) CHECK(!call->segment && accounting.live_bytes == call->allocation_bytes);
    } else if (!call) CHECK(fail_at != SIZE_MAX && status == XR_XIR_CALL_OOM);
    size_t sites = calls;
    CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
    CHECK(!live && !accounting.live_bytes && !accounting.depth && accounting.allocations == accounting.frees);
    if (fail_at == SIZE_MAX) {
        CHECK(witness.cleanups == (mode == 3 ? 1u : mode ? 49u : 48 * passes + 1));
        printf("Segment frames: state=%u mode=%u passes=%u peak=%llu allocations=%llu cleanups=%u\n",
            bytes, mode, passes, (unsigned long long) accounting.peak_bytes,
            (unsigned long long) accounting.allocations, witness.cleanups);
    }
    return sites;
}
static void segment_budget_cases(void) {
    SegmentWitness witness = {0}; witness.state_bytes = 131; witness.passes = 1;
    XrXirCallEntry entry = segment_entry(131); XrXirCallAccounting accounting = {0};
    XrXirCallConfig config = {&entry, 1, &witness, 65536, 100, 2, &accounting, {0}};
    uint64_t metadata = 0; CHECK(table_size(&config, &metadata) == XR_XIR_CALL_READY);
    uint64_t minimum = metadata + frame_align(sizeof(CallSegment)) +
        frame_align(state_offset() + ((uint64_t) entry.state_bytes + 7) / 8 * 8 + sizeof(XrXirValue)) + 16;
    const uint64_t limits[] = {metadata - 1, minimum - 1, minimum, minimum + 1, metadata + 4095, metadata + 4096};
    XrXirValue argument = {XR_XIR_I64, 0, 0};
    for (unsigned i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        memset(&accounting, 0, sizeof(accounting)); config.byte_limit = limits[i];
        XrXirCall *call = NULL;
        XrXirCallStatus status = xr_xir_call_new(&config, 0, &argument, 1, &call);
        if (i < 2) CHECK(status == XR_XIR_CALL_LIMIT && !call);
        else {
            CHECK(status == XR_XIR_CALL_READY && accounting.live_bytes <= limits[i]);
            CHECK(call->segment->allocation_bytes == (limits[i] - metadata) / 16 * 16);
            CHECK(xr_xir_call_poll(call).status == XR_XIR_CALL_SUSPENDED);
        }
        witness.cleanups = 0; witness.last_cleanup = UINT32_MAX;
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        CHECK(!live && !accounting.live_bytes && accounting.allocations == accounting.frees);
    }
}
typedef struct SegmentCopyWitness { XrXirValue arguments[2]; unsigned cleanups; } SegmentCopyWitness;
static XrXirAction segment_copy_resume(XrXirCallView *view) {
    SegmentCopyWitness *w = view->instance;
    CHECK(!view->argument_count);
    return (XrXirAction) {XR_XIR_ACTION_CALL, 1, w->arguments, 2, {0}};
}
static void segment_copy_cleanup(XrXirCallView *view, XrXirCallStatus reason) {
    SegmentCopyWitness *w = view->instance;
    CHECK(!view->argument_count && reason == XR_XIR_CALL_LIMIT); ++w->cleanups;
}
static void segment_copy_failure(void) {
    XrXirDomain *domain = NULL; SegmentCopyWitness witness = {0};
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "first", 5, &witness.arguments[0]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "second", 6, &witness.arguments[1]) == XR_XIR_VALUE_OK);
    XirObject *first = object_pointer(&witness.arguments[0]), *second = object_pointer(&witness.arguments[1]);
    atomic_store(&second->references, UINT32_MAX);
    XrXirType types[2] = {XR_XIR_STRING, XR_XIR_STRING};
    size_t baseline = live;
    for (unsigned large = 0; large < 2; ++large) {
        XrXirCallEntry entries[] = {
            {XR_XIR_CALL_ABI_VERSION, NULL, 0, XR_XIR_UNIT, 16, segment_copy_resume, segment_copy_cleanup, NULL},
            {XR_XIR_CALL_ABI_VERSION, types, 2, XR_XIR_UNIT, large ? 8193u : 16u, segment_copy_resume, segment_copy_cleanup, NULL}
        };
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config = {entries, 2, &witness, 65536, 10, 4, &accounting, {0}};
        XrXirCall *call = NULL;
        CHECK(xr_xir_call_new(&config, 0, NULL, 0, &call) == XR_XIR_CALL_READY);
        CHECK(xr_xir_call_poll(call).status == XR_XIR_CALL_LIMIT && witness.cleanups == large + 1);
        CHECK(!call->segment && accounting.live_bytes == call->allocation_bytes);
        CHECK(atomic_load(&first->references) == 1 && atomic_load(&second->references) == UINT32_MAX);
        CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY && live == baseline);
        CHECK(!accounting.live_bytes && accounting.allocations == accounting.frees);
    }
    atomic_store(&second->references, 1);
    xr_xir_value_drop(&witness.arguments[0]); xr_xir_value_drop(&witness.arguments[1]);
    xr_xir_domain_drop(domain); CHECK(!live);
}
static void segment_cases(void) {
    fail_at = SIZE_MAX; segment_budget_cases(); segment_copy_failure();
    for (uint32_t large = 0; large < 2; ++large) {
        uint32_t bytes = large ? 8193 : 131;
        for (uint32_t mode = 0; mode < 5; ++mode) {
            fail_at = SIZE_MAX; calls = 0; segment_attempt(bytes, mode, 1);
        }
        calls = 0; size_t sites = segment_attempt(bytes, 0, 4);
        for (size_t i = 0; i < sites; ++i) {
            fail_at = i; calls = 0; segment_attempt(bytes, 0, 4);
        }
        fail_at = SIZE_MAX;
        printf("Segment frame allocation failures: state=%u sites=%zu\n", bytes, sites);
    }
}
#endif // XIR_SEGMENT_CASES_H
