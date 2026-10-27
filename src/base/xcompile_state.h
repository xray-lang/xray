/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompile_state.h - Request-local parser failure and memory ownership
 */
#ifndef XCOMPILE_STATE_H
#define XCOMPILE_STATE_H

#include "xcompile_resources.h"

typedef struct XrCompileState XrCompileState;

/* One request owns this parser failure record; never publish it in an artifact
 * or share it with a new request. Parser copies share its first failure, which
 * cannot be reset. The state retains memory until its last reference dies.
 * Constructors preserve nonempty outputs and never create a new memory owner.
 * Producer operations are caller-serialized; reference/status may cross threads. */
XR_FUNC XrCompileResourceStatus xr_compile_state_new(XrCompileResources *resources, XrCompileState **output);
XR_FUNC XrCompileResourceStatus xr_compile_state_retain(XrCompileState *state);
XR_FUNC void xr_compile_state_release(XrCompileState *state);
/* Parser sessions register their real lifetime separately from retained ASTs.
 * A detached owning AST may cross sessions on its original ledger; an active
 * parser state remains exclusive to that exact state. */
XR_FUNC XrCompileResourceStatus xr_compile_state_session_open(XrCompileState *state);
XR_FUNC void xr_compile_state_session_close(XrCompileState *state);
XR_FUNC bool xr_compile_state_has_sessions(const XrCompileState *state);
XR_FUNC XrCompileResources *xr_compile_state_resources(const XrCompileState *state);
XR_FUNC XrCompileResourceStatus xr_compile_state_status(const XrCompileState *state);
XR_FUNC XrCompileResourceStatus xr_compile_state_fail(XrCompileState *state, XrCompileResourceStatus status);
XR_FUNC XrCompileResourceStatus xr_compile_state_alloc(XrCompileState *state, size_t bytes, void **output);
XR_FUNC XrCompileResourceStatus xr_compile_state_calloc(XrCompileState *state, size_t count, size_t size, void **output);
XR_FUNC XrCompileResourceStatus xr_compile_state_resize(XrCompileState *state, void **memory, size_t bytes);
/* Cleanup remains available after failure and returns physical held bytes. */
XR_FUNC void xr_compile_state_free(void *memory);
XR_FUNC XrCompileResourceStatus xr_compile_state_copy(XrCompileState *state, void *destination, const void *source, size_t bytes);
XR_FUNC XrCompileResourceStatus xr_compile_state_zero(XrCompileState *state, void *destination, size_t bytes);
/* The caller supplies a readable, terminated string and polls at safe points. */
XR_FUNC XrCompileResourceStatus xr_compile_state_string_length(XrCompileState *state, const char *text, size_t *output);
XR_FUNC XrCompileResourceStatus xr_compile_state_strndup(XrCompileState *state, const char *text, size_t length, char **output);
XR_FUNC XrCompileResourceStatus xr_compile_state_strdup(XrCompileState *state, const char *text, char **output);

#endif // XCOMPILE_STATE_H
