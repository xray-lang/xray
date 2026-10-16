/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_rendezvous_internal.h - Owned zero-capacity send publication
 *
 * KEY CONCEPT:
 *   A parked payload owns storage and no callback, frame or admission borrow.
 */
#ifndef XXIR_CHANNEL_RENDEZVOUS_INTERNAL_H
#define XXIR_CHANNEL_RENDEZVOUS_INTERNAL_H
#include "xxir_channel.h"
typedef struct XrXirChannelSendWait XrXirChannelSendWait;
typedef struct XrXirChannelRecvWait XrXirChannelRecvWait;
typedef enum XrXirChannelSendWaitState {
    XR_XIR_CHANNEL_SEND_WAIT_RETRY, XR_XIR_CHANNEL_SEND_WAIT_PENDING,
    XR_XIR_CHANNEL_SEND_WAIT_SENT, XR_XIR_CHANNEL_SEND_WAIT_CLOSED,
    XR_XIR_CHANNEL_SEND_WAIT_CANCELLED
} XrXirChannelSendWaitState;
/* Only zero-capacity Channels use this intent. PENDING publishes one unique
 * owner; RETRY and CLOSED publish none. The ordinary message is never consumed.
 * A successful intent publishes its actual Class identities before parking. */
XR_FUNC XrXirValueStatus xr_xir_channel_rendezvous_send_begin(const XrXirValue *channel,
    const XrXirValue *message, XrXirValueAdmission *admission,
    XrXirChannelSendWait **output, XrXirChannelSendWaitState *state);
XR_FUNC XrXirChannelSendWaitState xr_xir_channel_send_wait_state(const XrXirChannelSendWait *wait);
XR_FUNC bool xr_xir_channel_send_wait_owner(const XrXirChannelSendWait *wait,const XrXirValueAdmission *admission);
/* The unique owner may withdraw a still-pending intent without releasing it.
 * True wins against peer acknowledgment and close. False preserves the first
 * accepted terminal state. Neither branch allocates or takes the Channel lock. */
XR_FUNC bool xr_xir_channel_send_wait_cancel_pending(XrXirChannelSendWait *wait);
/* Drop consumes the unique owner without waiting for the Channel lock. Queue
 * storage retains any cancelled payload until safe unlink or final retirement. */
XR_FUNC void xr_xir_channel_send_wait_drop(XrXirChannelSendWait *wait);

typedef enum XrXirChannelRecvWaitState {
    XR_XIR_CHANNEL_RECV_WAIT_RETRY, XR_XIR_CHANNEL_RECV_WAIT_PENDING,
    XR_XIR_CHANNEL_RECV_WAIT_VALUE, XR_XIR_CHANNEL_RECV_WAIT_CLOSED,
    XR_XIR_CHANNEL_RECV_WAIT_CANCELLED
} XrXirChannelRecvWaitState;
/* Only empty zero-capacity Channels can park a receiver. A live pending sender
 * returns RETRY so the caller takes its normal owned receive path first.
 * PENDING owns unpublished Recv storage and a prepared Closed value, never an
 * admission or execution callback. The Channel element's definition-context
 * Sendable obligation must already have been discharged. */
XR_FUNC XrXirValueStatus xr_xir_channel_rendezvous_recv_begin(const XrXirValue *channel,
    XrXirType result_type, XrXirValueAdmission *admission,
    XrXirChannelRecvWait **output, XrXirChannelRecvWaitState *state);
/* Bare receive owns no Recv wrapper allocation or empty-variant descriptor.
 * VALUE takes the actual payload, including canonical Unit; CLOSED takes Unit.
 * The caller retains its already evaluated default for the CLOSED branch. */
XR_FUNC XrXirValueStatus xr_xir_channel_rendezvous_recv_raw_begin(const XrXirValue *channel,
    XrXirValueAdmission *admission,XrXirChannelRecvWait **output,XrXirChannelRecvWaitState *state);
XR_FUNC XrXirChannelRecvWaitState xr_xir_channel_recv_wait_state(const XrXirChannelRecvWait *wait);
XR_FUNC bool xr_xir_channel_recv_wait_owner(const XrXirChannelRecvWait *wait,const XrXirValueAdmission *admission);
XR_FUNC bool xr_xir_channel_recv_wait_cancel_pending(XrXirChannelRecvWait *wait);
/* The unique owner may take a ready result once into canonical Unit storage.
 * This move and cancellation allocate nothing and never wait for the lock. */
XR_FUNC bool xr_xir_channel_recv_wait_take(XrXirChannelRecvWait *wait,XrXirValue *output);
XR_FUNC void xr_xir_channel_recv_wait_drop(XrXirChannelRecvWait *wait);
#endif // XXIR_CHANNEL_RENDEZVOUS_INTERNAL_H
