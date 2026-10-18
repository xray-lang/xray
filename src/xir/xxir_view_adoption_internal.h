/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_adoption_internal.h - Prepaid adoption before an actual root ends
 *
 * KEY CONCEPT:
 *   A real driver proves the receiving scope before changing a live guard.
 */
#ifndef XXIR_VIEW_ADOPTION_INTERNAL_H
#define XXIR_VIEW_ADOPTION_INTERNAL_H
#include "xxir_view_value_internal.h"
/* Authenticates the actual whole-Cell inline loan and prepays its detachment.
 * This does not validate returned leaves or grant a Source lifetime proof. */
XR_FUNC XrXirValueStatus xr_xir_view_ref_return_prepare(const XrXirValue *root,
    const XrXirCellLoan *loan,const XrXirCellAuthority *authority,XrXirValueAdmission *admission);
/* Only after every abandoned child is retired and every returned leaf adopted. */
XR_FUNC void xr_xir_view_ref_return_commit(XrXirCellLoan *loan);
/* The driver has authenticated this exact returned leaf and the real caller.
 * Shared guards rebind; mutable bindings already have stable unique authority. */
XR_FUNC void xr_xir_view_scope_adopt(XrXirViewOriginPin *pin,const XrXirCellAuthority *authority);
#endif // XXIR_VIEW_ADOPTION_INTERNAL_H
