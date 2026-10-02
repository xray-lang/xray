/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompile_state.c - Sticky resource failure independent of producer lifetime
 */
#include "xcompile_state.h"
#include "xchecks.h"
#include <stdatomic.h>
#include <string.h>

struct XrCompileState {
    XrCompileResources *resources;
    _Atomic uint64_t references;
    _Atomic int failure;
};

XR_FUNC XrCompileResourceStatus xr_compile_state_new(XrCompileResources *resources, XrCompileState **output) {
    if (!resources || !output || *output) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    void *memory = NULL;
    XrCompileResourceStatus status = xr_compile_resources_alloc(resources, sizeof(XrCompileState), &memory);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    XrCompileState *state = memory;
    state->resources = resources;
    atomic_init(&state->references, 1);
    atomic_init(&state->failure, XR_COMPILE_RESOURCE_OK);
    *output = state;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xr_compile_state_status(const XrCompileState *state) {
    return state ? (XrCompileResourceStatus) atomic_load_explicit(&state->failure, memory_order_acquire)
                 : XR_COMPILE_RESOURCE_BAD_ARGUMENT;
}

XR_FUNC XrCompileResourceStatus xr_compile_state_fail(XrCompileState *state, XrCompileResourceStatus status) {
    if (!state) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if (status < XR_COMPILE_RESOURCE_OK || status > XR_COMPILE_RESOURCE_OUT_OF_MEMORY)
        status = XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if (status != XR_COMPILE_RESOURCE_OK) {
        int expected = XR_COMPILE_RESOURCE_OK;
        atomic_compare_exchange_strong_explicit(&state->failure, &expected, status,
            memory_order_acq_rel, memory_order_acquire);
    }
    return xr_compile_state_status(state);
}

XR_FUNC XrCompileResourceStatus xr_compile_state_retain(XrCompileState *state) {
    if (!state) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    uint64_t current = atomic_load_explicit(&state->references, memory_order_relaxed);
    for (;;) {
        if (current == UINT64_MAX) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
        if (atomic_compare_exchange_weak_explicit(&state->references, &current, current + 1,
                memory_order_relaxed, memory_order_relaxed)) return XR_COMPILE_RESOURCE_OK;
    }
}

XR_FUNC void xr_compile_state_release(XrCompileState *state) {
    if (!state) return;
    uint64_t old = atomic_fetch_sub_explicit(&state->references, 1, memory_order_acq_rel);
    XR_CHECK(old != 0, "Compiler state release requires a live reference");
    if (old == 1) xr_compile_resources_free(state);
}

XR_FUNC XrCompileResources *xr_compile_state_resources(const XrCompileState *state) {
    return state ? state->resources : NULL;
}

XR_FUNC XrCompileResourceStatus xr_compile_state_work(XrCompileState *state, uint64_t units) {
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    return xr_compile_state_fail(state, xr_compile_resources_work(state->resources, units));
}

XR_FUNC XrCompileResourceStatus xr_compile_state_alloc(XrCompileState *state, size_t bytes, void **output) {
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    return xr_compile_state_fail(state, xr_compile_resources_alloc(state->resources, bytes, output));
}

XR_FUNC XrCompileResourceStatus xr_compile_state_calloc(XrCompileState *state, size_t count, size_t size, void **output) {
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    return xr_compile_state_fail(state, xr_compile_resources_calloc(state->resources, count, size, output));
}

XR_FUNC XrCompileResourceStatus xr_compile_state_resize(XrCompileState *state, void **memory, size_t bytes) {
    XrCompileResourceStatus status = xr_compile_state_status(state);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    return xr_compile_state_fail(state, xr_compile_resources_resize(state->resources, memory, bytes));
}

XR_FUNC void xr_compile_state_free(void *memory) {
    xr_compile_resources_free(memory);
}

XR_FUNC XrCompileResourceStatus xr_compile_state_copy(XrCompileState *state, void *destination, const void *source, size_t bytes) {
    if (bytes && (!source || !destination)) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XrCompileResourceStatus status = xr_compile_state_work(state, bytes);
    if (status == XR_COMPILE_RESOURCE_OK && bytes) memcpy(destination, source, bytes);
    return status;
}

XR_FUNC XrCompileResourceStatus xr_compile_state_zero(XrCompileState *state, void *destination, size_t bytes) {
    if (bytes && !destination) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    XrCompileResourceStatus status = xr_compile_state_work(state, bytes);
    if (status == XR_COMPILE_RESOURCE_OK && bytes) memset(destination, 0, bytes);
    return status;
}

XR_FUNC XrCompileResourceStatus xr_compile_state_string_length(XrCompileState *state, const char *text, size_t *output) {
    if (!text || !output) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    for (size_t length = 0;; ++length) {
        XrCompileResourceStatus status = xr_compile_state_work(state, 1);
        if (status != XR_COMPILE_RESOURCE_OK) return status;
        if (!text[length]) {
            *output = length;
            return XR_COMPILE_RESOURCE_OK;
        }
        if (length == SIZE_MAX - 1) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
    }
}

XR_FUNC XrCompileResourceStatus xr_compile_state_strndup(XrCompileState *state, const char *text, size_t length, char **output) {
    if (!output || *output || (!text && length)) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    if (length == SIZE_MAX) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
    void *memory = NULL;
    XrCompileResourceStatus status = xr_compile_state_alloc(state, length + 1, &memory);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = xr_compile_state_copy(state, memory, text, length);
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_state_work(state, 1);
    if (status != XR_COMPILE_RESOURCE_OK) {
        xr_compile_resources_free(memory);
        return status;
    }
    ((char *) memory)[length] = '\0';
    *output = memory;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xr_compile_state_strdup(XrCompileState *state, const char *text, char **output) {
    if (!output || *output) return xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    size_t length = 0;
    XrCompileResourceStatus status = xr_compile_state_string_length(state, text, &length);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    return xr_compile_state_strndup(state, text, length, output);
}
