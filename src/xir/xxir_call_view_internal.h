/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_view_internal.h - Views bound to actual driver frames and Cell loans
 *
 * KEY CONCEPT:
 *   The driver registers every constructed view and revokes it before root loans
 *   retire. A borrowed access object never owns a frame or permits an escape.
 */
#ifndef XXIR_CALL_VIEW_INTERNAL_H
#define XXIR_CALL_VIEW_INTERNAL_H
#include "xxir_call.h"
#include "xxir_view_value_internal.h"

/* Outputs are canonical empty Value slots in the actual active frame state.
 * The root parameter must have that frame's committed whole-Cell ref loan.
 * No caller-supplied authority, binding flag or copied callback is accepted. */
XR_FUNC XrXirValueStatus xr_xir_call_view_new(XrXirCallView *view,
    XrXirType type,uint32_t parameter,int64_t offset,int64_t length,XrXirValue *output);
/* A local root is an owned unpublished whole Cell in this actual frame's state. Existing
 * exclusive loans cannot be bypassed through a copied Cell handle. The native
 * producer owns the state values; this grants no Source binding/NLL proof. */
XR_FUNC XrXirValueStatus xr_xir_call_view_local(XrXirCallView *view,
    XrXirType type,const XrXirValue *root,int64_t offset,int64_t length,XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_call_view_reborrow(XrXirCallView *view,
    XrXirType type,const XrXirValue *parent,int64_t offset,int64_t length,XrXirValue *output);
/* The caller keeps the Call alive while using this borrowed object. Every use
 * rechecks its actual active callback and that frame's registered body identity;
 * retaining this record cannot keep a popped frame or its views authorized. */
XR_FUNC XrXirValueStatus xr_xir_call_view_access(const XrXirCallView *view,XrXirViewAccess *output);
/* Consumes an actual owned borrowed-value slot after all fallible work. Closed
 * registry entries are reclaimed; live aliases keep their existing scope. */
XR_FUNC XrXirValueStatus xr_xir_call_view_drop(XrXirCallView *view,XrXirValue *value);
/* Moves one prepared borrowed argument into an actual owned frame state slot.
 * Cell/ref arguments stay in the driver until their committed loans retire. */
XR_FUNC XrXirValueStatus xr_xir_call_view_argument_take(XrXirCallView *view,
    uint32_t parameter,XrXirValue *output);
/* Takes the already adopted returned owner without copying a mutable view. */
XR_FUNC XrXirValueStatus xr_xir_call_view_inbox_take(XrXirCallView *view,
    XrXirType type,XrXirValue *output);
#endif // XXIR_CALL_VIEW_INTERNAL_H
