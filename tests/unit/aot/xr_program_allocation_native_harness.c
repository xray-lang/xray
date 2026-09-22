/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_allocation_native_harness.c - Observe the emitted allocator unchanged
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocation.facts.h"

typedef struct AllocationRecord {
    void *base;
    size_t size;
    unsigned freed;
} AllocationRecord;

static AllocationRecord records[16];
static size_t attempts, allocated, released, last_size, fail_at;
static int bad_release;
static int (*before_first_free)(void *context);
static void *observation_context;
static unsigned observations;
static int observation_failure;

static void *observed_malloc(size_t size) {
    ++attempts;
    last_size = size;
    if (attempts == fail_at)
        return NULL;
    if (allocated >= sizeof(records) / sizeof(records[0]) || size > 4096u)
        return NULL;
    void *memory = malloc(size);
    if (memory) {
        records[allocated].base = memory;
        records[allocated].size = size;
        ++allocated;
    }
    return memory;
}

static void observed_free(void *memory) {
    if (before_first_free) {
        int (*observe)(void *) = before_first_free;
        before_first_free = NULL;
        ++observations;
        observation_failure = observe(observation_context);
    }
    for (size_t index = 0u; index < allocated; ++index) {
        if (records[index].base == memory && records[index].freed == 0u) {
            records[index].freed = 1u;
            ++released;
            free(memory);
            return;
        }
    }
    bad_release = 1;
}

/* Only the native allocator calls are intercepted; its body is generated code. */
#define malloc observed_malloc
#define free observed_free
#include "allocation.generated.c"
#undef free
#undef malloc

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "allocation check failed at %d: %s\n", __LINE__, #condition);          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static void reset_observation(void) {
    memset(records, 0, sizeof(records));
    attempts = allocated = released = last_size = fail_at = 0u;
    bad_release = 0;
    before_first_free = NULL;
    observation_context = NULL;
    observations = 0u;
    observation_failure = 0;
}

static int check_destroy(XrAotContext *context, size_t expected) {
    xr_aot_context_destroy(context);
    CHECK(context->allocations == NULL);
    CHECK(released == expected && !bad_release);
    for (size_t index = 0u; index < allocated; ++index)
        CHECK(records[index].freed == 1u);
    xr_aot_context_destroy(context);
    CHECK(released == expected && !bad_release);
    return 0;
}

static int check_live_typed_payloads(void *opaque) {
    (void) opaque;
    CHECK(attempts == 5u && allocated == 5u && released == 0u);
    size_t payload_sizes[] = {sizeof(XR_FIXTURE_PLAIN), sizeof(XR_FIXTURE_INNER),
                              sizeof(XR_FIXTURE_CAPTURE), sizeof(XR_FIXTURE_CAPTURE),
                              sizeof(XR_FIXTURE_INNER)};
    size_t payload_alignments[] = {_Alignof(XR_FIXTURE_PLAIN), _Alignof(XR_FIXTURE_INNER),
                                   _Alignof(XR_FIXTURE_CAPTURE), _Alignof(XR_FIXTURE_CAPTURE),
                                   _Alignof(XR_FIXTURE_INNER)};
    void *payloads[5];
    for (size_t index = 0u; index < 5u; ++index) {
        CHECK(records[index].size == sizeof(XrAotAllocation) + payload_sizes[index]);
        payloads[index] = (void *) ((XrAotAllocation *) records[index].base + 1);
        CHECK((uintptr_t) payloads[index] % payload_alignments[index] == 0u);
    }
    CHECK(((XR_FIXTURE_PLAIN *) payloads[0])->f0 == 42);
    CHECK(((XR_FIXTURE_INNER *) payloads[1])->f0 == 42);
    XR_FIXTURE_CAPTURE *original = (XR_FIXTURE_CAPTURE *) payloads[2];
    XR_FIXTURE_CAPTURE *copy = (XR_FIXTURE_CAPTURE *) payloads[3];
    CHECK(original != copy);
    CHECK(original->f0.tag == 0u && copy->f0.tag == 0u);
    CHECK(original->f1.f0 == 42 && copy->f1.f0 == 42);
    CHECK(original->f0.payload.case_0.f0.capture == payloads[1]);
    CHECK(copy->f0.payload.case_0.f0.capture == payloads[4]);
    CHECK(original->f0.payload.case_0.f0.capture != copy->f0.payload.case_0.f0.capture);
    CHECK(((XR_FIXTURE_INNER *) copy->f0.payload.case_0.f0.capture)->f0 == 42);
    ((XR_FIXTURE_INNER *) original->f0.payload.case_0.f0.capture)->f0 = 88;
    CHECK(((XR_FIXTURE_INNER *) copy->f0.payload.case_0.f0.capture)->f0 == 42);
    CHECK(original->f2 == 42 && copy->f2 == 42);
    original->f2 = 77;
    CHECK(copy->f2 == 42);
    return 0;
}

static int check_live_failed_copy_publication(void *opaque) {
    XrAotContext *context = opaque;
    CHECK(allocated == 5u && released == 0u);
    XR_FIXTURE_CAPTURE *source = (XR_FIXTURE_CAPTURE *) ((XrAotAllocation *) records[2].base + 1);
    XR_FIXTURE_CAPTURE destination = {0};
    destination.f2 = -7;
    XrAotAllocation *previous = context->allocations;
    fail_at = attempts + 1u;
    CHECK(!XR_FIXTURE_COPY_CAPTURE(context, source, &destination));
    CHECK(attempts == 6u && allocated == 5u && context->allocations == previous);
    CHECK(destination.f2 == -7 && destination.f0.payload.case_0.f0.capture == NULL);
    CHECK(source->f2 == 42 &&
          ((XR_FIXTURE_INNER *) source->f0.payload.case_0.f0.capture)->f0 == 42);
    return 0;
}

static int check_allocation_rejections(void) {
    reset_observation();
    XrAotContext context = {0};
    CHECK(xr_aot_alloc(NULL, 1u) == NULL && attempts == 0u);
    CHECK(xr_aot_alloc(&context, 1u) != NULL && allocated == 1u);
    XrAotAllocation *previous = context.allocations;
    size_t maximum = SIZE_MAX - sizeof(XrAotAllocation);
    size_t before = attempts;
    CHECK(xr_aot_alloc(&context, maximum + 1u) == NULL);
    CHECK(xr_aot_alloc(&context, SIZE_MAX) == NULL);
    CHECK(attempts == before && context.allocations == previous);
    fail_at = attempts + 1u;
    CHECK(xr_aot_alloc(&context, maximum) == NULL);
    CHECK(attempts == before + 1u && last_size == SIZE_MAX);
    CHECK(context.allocations == previous && allocated == 1u);
    fail_at = attempts + 1u;
    CHECK(xr_aot_alloc(&context, sizeof(XR_FIXTURE_CAPTURE)) == NULL);
    CHECK(attempts == before + 2u && context.allocations == previous && allocated == 1u);
    fail_at = 0u;
    /* A zero payload is not dereferenced; the header still belongs to the context. */
    CHECK(xr_aot_alloc(&context, 0u) != NULL);
    CHECK(allocated == 2u && last_size == sizeof(XrAotAllocation));
    return check_destroy(&context, 2u);
}

static int check_live_copy_dispatch_boundaries(void *opaque) {
    XrAotContext *context = opaque;
    CHECK(attempts == 5u && allocated == 5u && released == 0u);
    const XR_FIXTURE_CAPTURE *original =
        (const XR_FIXTURE_CAPTURE *) ((XrAotAllocation *) records[2].base + 1);
    XR_FIXTURE_CAPTURE source = *original;
    XR_FIXTURE_CAPTURE result = {0};
    source.f0.tag = 1u;
    CHECK(XR_FIXTURE_COPY_CAPTURE(context, &source, &result));
    CHECK(result.f0.tag == 1u && result.f1.f0 == 42 && result.f2 == 42);
    CHECK(attempts == 5u && allocated == 5u);
    for (unsigned invalid = 0u; invalid < 3u; ++invalid) {
        source = *original;
        result = (XR_FIXTURE_CAPTURE) {0};
        result.f2 = -7;
        if (invalid == 0u)
            source.f0.tag = UINT32_MAX;
        else if (invalid == 1u)
            source.f0.payload.case_0.f0.function_id = UINT32_MAX;
        else
            source.f0.payload.case_0.f0.capture = NULL;
        CHECK(!XR_FIXTURE_COPY_CAPTURE(context, &source, &result));
        CHECK(result.f2 == -7 && result.f0.payload.case_0.f0.capture == NULL);
        CHECK(attempts == 5u && allocated == 5u && released == 0u);
    }
    CHECK(original->f0.tag == 0u && original->f2 == 42);
    CHECK(((XR_FIXTURE_INNER *) original->f0.payload.case_0.f0.capture)->f0 == 42);
    return 0;
}

static int check_typed_program_observation(int (*observe)(void *)) {
    reset_observation();
    XrAotContext context = {0};
    before_first_free = observe;
    observation_context = &context;
    XrAotOutcome result = XR_FIXTURE_ENTRY(&context);
    CHECK(result.kind == 0u && result.value_kind == 2u && result.i64 == 42);
    CHECK(observations == 1u && observation_failure == 0);
    /* Both callable captures and their nested captures are physically gone;
     * the borrowed
     * existential's box still belongs to the entry arena. */
    CHECK(allocated == 5u && released == 4u && !bad_release);
    CHECK(records[0].freed == 0u);
    for (size_t index = 1u; index < 5u; ++index)
        CHECK(records[index].freed == 1u);
    CHECK(context.allocations == (XrAotAllocation *) records[0].base);
    return check_destroy(&context, 5u);
}

static int check_each_producer_failure(void) {
    for (size_t index = 1u; index <= 5u; ++index) {
        reset_observation();
        fail_at = index;
        XrAotContext context = {0};
        XrAotOutcome result = XR_FIXTURE_ENTRY(&context);
        CHECK(result.kind == 4u);
        CHECK(attempts == index && allocated == index - 1u);
        /* Exceptional exits release every acquired callable owner immediately.
         * Only the
         * read existential's non-owning arena box may remain. */
        CHECK(released == (index >= 2u ? index - 2u : 0u) && !bad_release);
        CHECK(context.allocations == (allocated ? (XrAotAllocation *) records[0].base : NULL));
        for (size_t owner = 1u; owner < allocated; ++owner)
            CHECK(records[owner].freed == 1u);
        CHECK(check_destroy(&context, index - 1u) == 0);
    }
    return 0;
}

int main(void) {
    if (check_typed_program_observation(check_live_typed_payloads) != 0 ||
        check_typed_program_observation(check_live_failed_copy_publication) != 0 ||
        check_typed_program_observation(check_live_copy_dispatch_boundaries) != 0 ||
        check_allocation_rejections() != 0 || check_each_producer_failure() != 0)
        return 1;
    puts("allocation-native: PASS allocations=5 producer_failures=5");
    return 0;
}
