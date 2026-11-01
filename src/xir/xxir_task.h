/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task.h - Owned task identity and repeated terminal observation
 *
 * KEY CONCEPT:
 *   An executor lease keeps pending work alive independently of public handles.
 */
#ifndef XXIR_TASK_H
#define XXIR_TASK_H
#include "xxir_call.h"
/* Copies a sticky terminal outcome. Pending or invalid tasks leave output unchanged. */
XR_FUNC XrXirCallStatus xr_xir_task_copy_outcome(const XrXirValue *task, XrXirCallResult *output);
/* Prepare the direct child completely before FIFO publication. The type comes
 * from this view's immutable Program arena; failure preserves an empty output. */
XR_FUNC XrXirCallStatus xr_xir_task_go(XrXirCallView *view, XrXirType task_type, uint32_t direct_entry,
    const XrXirValue *arguments, uint32_t count, XrXirValue *output);
/* Request cancellation of this authenticated executor member without driving it. */
XR_FUNC XrXirCallStatus xr_xir_task_cancel(XrXirCallView *view,const XrXirValue *task);
typedef struct XrXirScopedSpawn {
    XrXirType type;
    uint32_t entry;
    const XrXirValue *arguments;
    uint32_t count;
} XrXirScopedSpawn;
/* Compiler-owned lexical tokens are real Tasks, but acquire members only from
 * their original live frame. Ordinary go never inherits a lexical group. */
XR_FUNC XrXirCallStatus xr_xir_task_scope_begin(XrXirCallView *view,
    XrXirType type,uint32_t mode,XrXirValue *output);
XR_FUNC XrXirCallStatus xr_xir_task_go_scoped(XrXirCallView *view,
    const XrXirValue *scope,const XrXirScopedSpawn *spawn,XrXirValue *output);
/* Sealing precedes one ordinary task wait. Abandon cancels children, without
 * manufacturing a Cancelled result for the lexical join itself. */
XR_FUNC XrXirCallStatus xr_xir_task_scope_seal(XrXirCallView *view,
    const XrXirValue *scope,bool abandon);
#endif // XXIR_TASK_H
