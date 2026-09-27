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

typedef struct XrXirTypes XrXirTypes;
typedef struct XrXirBudget XrXirBudget;

/* Success spends the owned metadata and validation/copy work budget. Failures
 * preserve the caller budget. Invalid or abstract pools reject before allocation;
 * NULL denotes an empty pool. */
XR_FUNC XrXirValueStatus xr_xir_type_arena_new(XrXirDomain *domain, const XrXirTypes *types,
    XrXirBudget *budget, XrXirTypeArena **output);
XR_FUNC bool xr_xir_type_arena_retain(XrXirTypeArena *arena);
XR_FUNC void xr_xir_type_arena_drop(XrXirTypeArena *arena);
XR_FUNC const XrXirTypes *xr_xir_type_arena_types(const XrXirTypeArena *arena);
#endif // XXIR_TYPE_ARENA_H
