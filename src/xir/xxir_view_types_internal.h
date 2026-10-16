/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_types_internal.h - Borrowed descriptor and signature restrictions
 *
 * KEY CONCEPT:
 *   Type shape restricts storage but never proves a live borrow or its origin.
 */
#ifndef XXIR_VIEW_TYPES_INTERNAL_H
#define XXIR_VIEW_TYPES_INTERNAL_H
#include "xxir.h"

#define XR_XIR_VIEW_SHARED 1u
#define XR_XIR_VIEW_MUTABLE 2u

/* Requires an admitted descriptor pool. Tuple and Optional preserve every
 * borrowed leaf; an owned callable's signature is not its capture storage.
 * Output is assigned only after the finite traversal succeeds. */
XR_FUNC XrXirStatus xr_xir_compile_view_type_flags(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirType type, uint32_t *output);
/* Declared callable parameters exclude physical hidden captures. This returns
 * the parameter of the unique signature candidate, not proof that any returned
 * leaf uses it. Each borrowed input leaf counts, including Tuple and Optional.
 * An ordinary result has the canonical UINT32_MAX candidate. */
XR_FUNC XrXirStatus xr_xir_compile_view_return_candidate(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirType callable, uint32_t *output);
/* Requires a structurally and semantically admitted module. No executable
 * borrowed value is admitted without independent CFG proof. */
XR_FUNC XrXirStatus xir_compile_view_module_boundary(const XrXirCompileContext *context,
    const XrXirModule *module);
#endif // XXIR_VIEW_TYPES_INTERNAL_H
