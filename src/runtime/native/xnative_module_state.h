/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_module_state.h - Shared typed state runtime implementation boundary
 *
 * KEY CONCEPT:
 *   VM and native execution consume the same owned slot and type operations.
 */
#ifndef XNATIVE_MODULE_STATE_H
#define XNATIVE_MODULE_STATE_H
#include "xray_native_state.h"
#include "xnative_value_image.h"
XR_FUNC XrtManagedStatus xrt_module_layout_plan(const XrtModuleLayout *, uint32_t type,
    const XrtValuePlan **empty);
/* A synchronous metadata borrow under the caller's live State owner. The same
 * bound plan can supply cold Env setup without binding another image. This
 * grants neither a payload owner nor invocation or mutation authority.
 * Foreign C callers of the State value operations already provide a complete
 * admitted value of the declared type. They must authenticate the backend
 * entry, signature and executable lifetime before passing a Function binding;
 * a non-NULL entry, Env ordinal or plan pointer does not establish that fact.
 * Owned Function bindings use the paired Env payload address as environment.
 * Frame borrows cannot be published into State. */
XR_FUNC XrtManagedStatus xrt_module_state_plan(const XrtModuleState *, uint32_t type,
    const XrtValuePlan **empty);
XR_FUNC XrtManagedStatus xrt_module_layout_inline_array(const XrtModuleLayout *, uint32_t type,
    XrtBoundInlineArray *empty);
XR_FUNC XrtManagedStatus xrt_module_state_inline_array(const XrtModuleState *, uint32_t type,
    XrtBoundInlineArray *empty);
#endif // XNATIVE_MODULE_STATE_H
