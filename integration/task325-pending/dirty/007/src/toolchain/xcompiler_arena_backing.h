/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler_arena_backing.h - Bind arena storage to an existing compiler owner
 */
#ifndef XCOMPILER_ARENA_BACKING_H
#define XCOMPILER_ARENA_BACKING_H

#include "base/xarena_backing.h"
#include "base/xcompile_state.h"

struct XrCompileResources;
struct XrCompileState;
struct XrArena;

/* The caller retains resources until arena open; the factory does not retain.
 * A failed binding leaves output unchanged. No default or fresh ledger exists. */
XR_FUNC XrArenaStatus xr_compiler_arena_backing(
    struct XrCompileResources *resources, XrArenaBacking *output);

/* The arena retains shared failure state independently of its producer. */
XR_FUNC XrArenaStatus xr_compiler_arena_state_backing(
    struct XrCompileState *state, XrArenaBacking *output);
XR_FUNC bool xr_compiler_arena_matches_state(
    const struct XrArena *arena, const struct XrCompileState *state);
XR_FUNC XrCompileResourceStatus xr_compiler_arena_capture_status(
    const struct XrArena *arena, struct XrCompileState *state);

#endif // XCOMPILER_ARENA_BACKING_H
