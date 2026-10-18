/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_call_internal.h - Common owned blocking Channel execution
 */
#ifndef XXIR_CHANNEL_CALL_INTERNAL_H
#define XXIR_CHANNEL_CALL_INTERNAL_H
#include "xxir_call.h"
#include "xxir_channel.h"
typedef enum XrXirChannelCallOperation {
    XR_XIR_CHANNEL_CALL_SEND=1,XR_XIR_CHANNEL_CALL_RECV,XR_XIR_CHANNEL_CALL_RECV_OR,
    XR_XIR_CHANNEL_CALL_SEND_TIMEOUT,XR_XIR_CHANNEL_CALL_RECV_TIMEOUT
} XrXirChannelCallOperation;
/* This state contains no owner, pointer or admission borrow. The Call itself
 * owns a pending node/watch. The caller preserves the same operation, closed
 * type, receiver and already evaluated ordinary argument across suspension. */
typedef struct XrXirChannelCallState {
    uint32_t waiting,operation;
    XrXirType result_type;
} XrXirChannelCallState;
/* The request is borrowed only for this synchronous step. The Call owns any
 * parked transport and the caller retains the already evaluated SSA values. */
typedef struct XrXirChannelCallRequest {
    XrXirChannelCallOperation operation;
    const XrXirValue *channel,*argument;
    XrXirType result_type;
    int64_t timeout_ms;
} XrXirChannelCallRequest;
/* READY publishes a complete result. SEND returns an I64 SENT/CLOSED transport
 * state; its caller implements the public value-error branch. RECV returns an
 * actual native Recv<T>, and RECV_OR returns T without a Recv allocation.
 * SUSPENDED requires an empty AWAIT_CHANNEL action; BUSY requires CONTINUE.
 * Output must be separate canonical Unit storage. No argument is consumed. */
XR_FUNC XrXirCallStatus xr_xir_call_channel_step(XrXirCallView *view,XrXirChannelCallState *state,
    const XrXirChannelCallRequest *request,XrXirValue *output);
#endif // XXIR_CHANNEL_CALL_INTERNAL_H
