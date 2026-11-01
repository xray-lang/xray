/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_select_internal.h - Typed results for one owned Channel frontier
 *
 * KEY CONCEPT:
 *   Requests expire with the synchronous step; the Call retains only an owned
 *   selection and an arena-local result type until its original frame resumes.
 */
#ifndef XXIR_CALL_SELECT_INTERNAL_H
#define XXIR_CALL_SELECT_INTERNAL_H
#include "xxir_call.h"
typedef struct XrXirChannelSelectCallState { uint32_t waiting; } XrXirChannelSelectCallState;
typedef struct XrXirChannelSelectCallRequest {
    const XrXirValue *channels;
    uint32_t count;
    XrXirType result_type;
    bool has_default,has_timeout;
    int64_t timeout_ms;
} XrXirChannelSelectCallRequest;
/* READY owns the exact tagged tuple. SUSPENDED requires AWAIT_CHANNEL, BUSY
 * requires CONTINUE at the same instruction. Neither result consumes a queue. */
XR_FUNC XrXirCallStatus xr_xir_call_select_step(XrXirCallView *view,
    XrXirChannelSelectCallState *state,const XrXirChannelSelectCallRequest *request,XrXirValue *output);
#endif // XXIR_CALL_SELECT_INTERNAL_H
