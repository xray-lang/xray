/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_arena.h - Independently owned immutable runtime type identities
 *
 * KEY CONCEPT:
 *   Metadata has its own lifetime and never retains a Program or Instance.
 */
#ifndef XXIR_TYPE_ARENA_H
#define XXIR_TYPE_ARENA_H
#include "xxir_value.h"
#include "xxir_storage.h"
#include "xxir_compile_context.h"

typedef struct XrXirTypes XrXirTypes;
/* The context owns actual metadata and all validation/copy work. Failures
 * preserve output; already performed work is not refunded. NULL types denotes
 * an empty pool, independently of the runtime allocation domain. */
XR_FUNC XrXirValueStatus xr_xir_compile_type_arena_new(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirTypeArena **output);
XR_FUNC bool xr_xir_compile_type_arena_retain(XrXirTypeArena *arena);
XR_FUNC void xr_xir_compile_type_arena_drop(XrXirTypeArena *arena);
XR_FUNC const XrXirTypes *xr_xir_compile_type_arena_types(const XrXirTypeArena *arena);
/* Borrowed immutable storage metadata; valid until the arena's last release. */
XR_FUNC const XrXirStorageLayout *xr_xir_compile_type_arena_storage(const XrXirTypeArena *arena,
    XrXirType type);
XR_FUNC bool xr_xir_compile_type_arena_layout(const XrXirTypeArena *arena,
    XrXirType type, XrXirLayout *layout);
#endif // XXIR_TYPE_ARENA_H
