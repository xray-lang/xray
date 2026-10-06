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
#endif // XXIR_TASK_H
