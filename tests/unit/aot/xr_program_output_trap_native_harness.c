/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_output_trap_native_harness.c - Native output refusal and allocation failure checks
 */

/* This file is appended to actual generated C by the source fixture producer. */
static size_t output_alloc_attempt, output_alloc_fail_at, output_alloc_live;
static int output_alloc_failed;

static void *output_trap_malloc(size_t size) {
    if (++output_alloc_attempt == output_alloc_fail_at) {
        output_alloc_failed = 1;
        return NULL;
    }
    void *pointer = malloc(size);
    if (!pointer)
        abort();
    ++output_alloc_live;
    return pointer;
}

static void output_trap_free(void *pointer) {
    if (!pointer)
        return;
    if (output_alloc_live == 0u)
        abort();
    --output_alloc_live;
    free(pointer);
}

typedef struct OutputTrapTrace {
    uint32_t calls, fail_at, constructed, finalized, reclaimed;
    uint64_t identities[2];
    size_t size;
    char bytes[128];
    int invalid;
} OutputTrapTrace;

static int output_trap_write(void *opaque, uint32_t requirement, uint32_t operation,
                             const uint8_t *bytes, size_t size) {
    OutputTrapTrace *trace = opaque;
    if (!trace || requirement != 0u || operation != 0u || !bytes ||
        size > sizeof(trace->bytes) - trace->size)
        return 1;
    if (++trace->calls == trace->fail_at)
        return 1;
    memcpy(trace->bytes + trace->size, bytes, size);
    trace->size += size;
    return 0;
}

static void output_trap_lifecycle(void *opaque, const XrAotLifecycleEvent *event) {
    OutputTrapTrace *trace = opaque;
    if (event->kind == 1u) {
        if (trace->constructed >= 2u) {
            trace->invalid = 1;
            return;
        }
        trace->identities[trace->constructed++] = event->identity;
    } else if (event->kind == 8u) {
        if (trace->finalized >= trace->constructed ||
            event->identity != trace->identities[trace->constructed - 1u - trace->finalized])
            trace->invalid = 1;
        ++trace->finalized;
    } else if (event->kind == 9u) {
        if (trace->reclaimed >= trace->constructed ||
            event->identity != trace->identities[trace->constructed - 1u - trace->reclaimed])
            trace->invalid = 1;
        ++trace->reclaimed;
    }
}

static int output_trap_run(uint32_t fail, size_t fail_allocation) {
    output_alloc_attempt = 0u;
    output_alloc_fail_at = fail_allocation;
    output_alloc_failed = 0;
    OutputTrapTrace trace = {0};
    trace.fail_at = fail;
    XrAotContext context = {0};
    XrAotModules modules = {0};
    modules.storage.modules = &modules;
    context.modules = &modules;
    context.provider_context = &trace;
    context.provider_output_write = output_trap_write;
    context.lifecycle_context = &trace;
    context.lifecycle_event = output_trap_lifecycle;
    modules.storage.lifecycle_context = &trace;
    modules.storage.lifecycle_event = output_trap_lifecycle;
    const void *error = NULL;
    for (uint32_t repeat = 0u; repeat < 24u; ++repeat) {
        XrAotOutcome result = xr_aot_initialize_modules(&context);
        /* Private initialization preserves OOM; only the public host projects trap4. */
        uint32_t kind = output_alloc_failed ? 4u : fail ? 1u : XR_OUTPUT_PANIC ? 3u : 2u;
        uint32_t trap = !output_alloc_failed && fail ? 7u : 0u;
        if (result.kind != kind || result.trap != trap)
            return 1;
        if (result.panic_present != (kind == 3u ? 1u : 0u) ||
            (kind == 3u && result.panic_info.code != 1u))
            return 7;
#if !XR_OUTPUT_PANIC
        if (kind == 2u) {
            if (result.error_type_id != XR_OUTPUT_ERROR_ID || !result.error_value)
                return 8;
            if (repeat == 0u)
                error = result.error_value;
            const XR_OUTPUT_ERROR_TYPE *cause = result.error_value;
            if (error != result.error_value || cause->tag != 0u || cause->payload.case_0.f0 != 7)
                return 9;
        }
#else
        (void)error;
#endif
        if (!output_alloc_failed && trace.calls != (fail == 1u ? 1u : 3u))
            return 2;
    }
    xr_aot_context_destroy(&context);
    xr_aot_modules_destroy(&modules);
    if (output_alloc_live != 0u || trace.invalid || trace.finalized != trace.constructed ||
        trace.reclaimed != trace.constructed)
        return 3;
    if (output_alloc_failed)
        return 0;
    const char *expected = fail == 1u   ? ""
                           : fail == 2u ? "library ready\ndone temporary\n"
                           : fail == 3u ? "library ready\ncleanup temporary\n"
                                        : "library ready\ncleanup temporary\ndone temporary\n";
    uint32_t count = fail == 1u ? 1u : 2u;
    if (trace.invalid || trace.constructed != count || trace.finalized != count ||
        trace.reclaimed != count || trace.size != strlen(expected) ||
        memcmp(trace.bytes, expected, trace.size) != 0)
        return 3;
    return 0;
}

int main(void) {
    for (uint32_t fail = 0u; fail < 4u; ++fail) {
        for (uint32_t separate = 0u; separate < 2u; ++separate) {
            if (output_trap_run(fail, 0u) != 0)
                return 4;
        }
        size_t allocations = output_alloc_attempt;
        if (allocations == 0u || allocations > 128u)
            return 5;
        fprintf(stderr, "Native output allocation failures: refusal=%u points=%zu\n", fail,
                allocations);
        for (size_t allocation = 1u; allocation <= allocations; ++allocation) {
            int result = output_trap_run(fail, allocation);
            if (result != 0 || !output_alloc_failed) {
                fprintf(stderr, "output failure=%u allocation=%zu result=%d live=%zu\n", fail,
                        allocation, result, output_alloc_live);
                return 6;
            }
        }
    }
    return 0;
}
