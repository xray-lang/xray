/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompile_state.h - Shared first-failure state with an owned compiler ledger
 */
#ifndef XCOMPILE_STATE_H
#define XCOMPILE_STATE_H

#include "xcompile_resources.h"

typedef struct XrCompileState XrCompileState;

/* The state allocation retains its ledger until the last state reference dies.
 * Constructors preserve nonempty outputs and never create a fresh ledger.
 * Fatal status is shared across parser copies and cannot be reset. Producer
 * operations on one state are serialized by its caller; reference ownership
 * and status observation may cross threads. */
XR_FUNC XrCompileResourceStatus xr_compile_state_new(XrCompileResources *resources, XrCompileState **output);
XR_FUNC XrCompileResourceStatus xr_compile_state_retain(XrCompileState *state);
XR_FUNC void xr_compile_state_release(XrCompileState *state);
XR_FUNC XrCompileResources *xr_compile_state_resources(const XrCompileState *state);
XR_FUNC XrCompileResourceStatus xr_compile_state_status(const XrCompileState *state);
XR_FUNC XrCompileResourceStatus xr_compile_state_fail(XrCompileState *state, XrCompileResourceStatus status);
XR_FUNC XrCompileResourceStatus xr_compile_state_work(XrCompileState *state, uint64_t units);
XR_FUNC XrCompileResourceStatus xr_compile_state_alloc(XrCompileState *state, size_t bytes, void **output);
XR_FUNC XrCompileResourceStatus xr_compile_state_calloc(XrCompileState *state, size_t count, size_t size, void **output);
XR_FUNC XrCompileResourceStatus xr_compile_state_resize(XrCompileState *state, void **memory, size_t bytes);
/* Cleanup always bypasses sticky failure and only returns physical live bytes. */
XR_FUNC void xr_compile_state_free(void *memory);
XR_FUNC XrCompileResourceStatus xr_compile_state_copy(XrCompileState *state, void *destination, const void *source, size_t bytes);
XR_FUNC XrCompileResourceStatus xr_compile_state_zero(XrCompileState *state, void *destination, size_t bytes);
/* Charge one byte before each read, including the terminating NUL. */
XR_FUNC XrCompileResourceStatus xr_compile_state_string_length(XrCompileState *state, const char *text, size_t *output);
XR_FUNC XrCompileResourceStatus xr_compile_state_strndup(XrCompileState *state, const char *text, size_t length, char **output);
XR_FUNC XrCompileResourceStatus xr_compile_state_strdup(XrCompileState *state, const char *text, char **output);

#endif // XCOMPILE_STATE_H
