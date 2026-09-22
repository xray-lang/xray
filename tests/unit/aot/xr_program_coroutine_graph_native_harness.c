/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_coroutine_graph_native_harness.c - Execute emitted coroutine graphs
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "graph.facts.h"
#include "graph.generated.c"

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "coroutine graph check failed at %d: %s\n", __LINE__, #condition);     \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int check_descriptor(void) {
    const uint8_t expected[] = XR_GRAPH_EXECUTION_ID;
    const XrBackendNativeDescriptor *descriptor = &xr_aot_entry_coroutine_descriptor;
    _Static_assert(sizeof(expected) == sizeof(descriptor->execution_id.bytes),
                   "execution identity width changed");
    CHECK(memcmp(expected, descriptor->execution_id.bytes, sizeof(expected)) == 0);
    CHECK(descriptor->schema_version == 9u && descriptor->reserved32 == 0u);
    CHECK(descriptor->frame_size == sizeof(XrAotEntryCoroutineFrame));
    CHECK(xr_aot_entry_coroutine_frame_size() == descriptor->frame_size);
    CHECK(descriptor->initialize && descriptor->step && descriptor->cancel && descriptor->drop);
    CHECK(descriptor->step(NULL).kind == 4u && descriptor->cancel(NULL).kind == 4u);
    descriptor->initialize(NULL, NULL, NULL);
    descriptor->drop(NULL);
    return 0;
}

#if XR_GRAPH_CLEANUP
_Static_assert(XR_GRAPH_CASES == 4, "cleanup mode census changed");

typedef struct ProviderTrace {
    int64_t token;
    uint32_t count;
    int refuse;
    int invalid;
} ProviderTrace;

static int poll_provider(void *opaque, uint32_t requirement, uint32_t operation, const XrProviderValuePack *arguments,
                         XrProviderValuePack *result) {
    ProviderTrace *trace = opaque;
    if (!arguments || arguments->count != 1u || arguments->nodes[0].token != 3u) return 1;
    int64_t token = arguments->nodes[0].as.i64;
    if (!trace)
        return 1;
    if (requirement != 0u || operation != 0u || !result || trace->count != 0u ||
        (token != 71 && token != 72)) {
        trace->invalid = 1;
        return 1;
    }
    trace->token = token;
    ++trace->count;
    if (trace->refuse)
        return 1;
    result->count = 1u; result->nodes[0].token = 3u;
    result->nodes[0].as.i64 = 99;
    return 0;
}

static void dispose_result(XrProviderValuePack *result) { *result = (XrProviderValuePack){0}; }

static int check_cleanup(void) {
    const XrBackendNativeDescriptor *descriptor = &xr_aot_entry_coroutine_descriptor;
    const char *names[] = {"resume", "resume-refusal", "cancel", "cancel-refusal"};
    for (uint32_t mode = 0u; mode < XR_GRAPH_CASES; ++mode) {
        int cancel = mode >= 2u;
        int refuse = (mode & 1u) != 0u;
        ProviderTrace trace = {.refuse = refuse};
        XrAotEntryCoroutineFrame frame;
        descriptor->initialize(&frame, NULL, NULL);
        frame.context.provider_context = &trace;
        frame.context.provider_call_typed = poll_provider;
        frame.context.provider_dispose_typed = dispose_result;
        XrBackendNativeOutcome paused = descriptor->step(&frame);
        CHECK(paused.kind == 1u && paused.state_id == 1u && paused.safepoint_id == 0u);
        CHECK(trace.count == 0u && !trace.invalid);
        XrBackendNativeOutcome outcome =
            cancel ? descriptor->cancel(&frame) : descriptor->step(&frame);
        CHECK(outcome.kind == (refuse ? 2u : cancel ? 3u : 0u));
        CHECK(outcome.value == 0 && outcome.safepoint_id == (refuse ? 7u : 0u));
        if (cancel && !refuse)
            CHECK(outcome.state_id == paused.state_id);
        CHECK(trace.count == 1u && trace.token == (cancel ? 72 : 71) && !trace.invalid);
        descriptor->drop(&frame);
        CHECK(frame.function.state == UINT32_MAX);
        descriptor->drop(&frame);
        CHECK(trace.count == 1u && !trace.invalid);
        printf("cleanup mode=%s trace=%lld outcome=%s\n", names[mode], (long long) trace.token,
               refuse   ? "trap7"
               : cancel ? "cancel"
                        : "return");
    }
    puts("coroutine-graph-native: PASS scenario=cleanup cases=4");
    return 0;
}
#else
_Static_assert(XR_GRAPH_CLEANUP == 0 && XR_GRAPH_CASES == 2, "branch mode census changed");

static int check_branch(void) {
    const XrBackendNativeDescriptor *descriptor = &xr_aot_entry_coroutine_descriptor;
    for (uint32_t cancel = 0u; cancel < XR_GRAPH_CASES; ++cancel) {
        XrAotEntryCoroutineFrame frame;
        descriptor->initialize(&frame, NULL, NULL);
        XrBackendNativeOutcome paused = descriptor->step(&frame);
        CHECK(paused.kind == 1u && paused.state_id == 1u && paused.safepoint_id == 0u);
        XrBackendNativeOutcome outcome =
            cancel ? descriptor->cancel(&frame) : descriptor->step(&frame);
        CHECK(outcome.kind == (cancel ? 3u : 0u));
        CHECK(outcome.value == (cancel ? 0 : 11) && outcome.safepoint_id == 0u);
        if (cancel)
            CHECK(outcome.state_id == paused.state_id);
        CHECK(frame.function.state == UINT32_MAX);
        descriptor->drop(&frame);
        descriptor->drop(&frame);
        CHECK(frame.function.state == UINT32_MAX);
        printf("branch mode=%s outcome=%s value=%lld\n", cancel ? "cancel" : "resume",
               cancel ? "cancel" : "return", (long long) outcome.value);
    }
    puts("coroutine-graph-native: PASS scenario=branch cases=2");
    return 0;
}
#endif

int main(void) {
    if (check_descriptor() != 0)
        return 1;
#if XR_GRAPH_CLEANUP
    return check_cleanup();
#else
    return check_branch();
#endif
}
