/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xblock.h - Backend-neutral coroutine blocking helpers
 */

#ifndef XBLOCK_H
#define XBLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "../runtime/value/xvalue.h"
#include "xchannel.h"

struct XrayIsolate;
struct XrCoroutine;

typedef enum {
    XR_SLOT_NONE = 0,
    XR_SLOT_XVALUE_PTR,
    XR_SLOT_AOT_FRAME_OFFSET,
    XR_SLOT_JIT_SUSPEND
} XrSlotKind;

typedef struct {
    XrSlotKind kind;
    void *base;
    uint32_t offset;
    uint16_t type_id;
} XrSlotRef;

typedef enum {
    XR_CORO_BLOCK_NOT_RESUMED = 0,
    XR_CORO_BLOCK_READY,
    XR_CORO_BLOCK_BLOCKED,
    XR_CORO_BLOCK_CLOSED,
    XR_CORO_BLOCK_TIMEOUT,
    XR_CORO_BLOCK_NO_CORO,
    XR_CORO_BLOCK_ERROR
} XrCoroBlockKind;

typedef struct {
    XrCoroBlockKind kind;
    XrValue value;
    bool ok;
} XrCoroBlockResult;

static inline XrSlotRef xr_slot_none(void) {
    XrSlotRef slot = {XR_SLOT_NONE, NULL, 0, 0};
    return slot;
}

static inline XrSlotRef xr_slot_xvalue_ptr(XrValue *ptr) {
    XrSlotRef slot = {XR_SLOT_XVALUE_PTR, ptr, 0, 0};
    return slot;
}

static inline XrSlotRef xr_slot_aot_frame_offset(void *base, uint32_t offset, uint16_t type_id) {
    XrSlotRef slot = {XR_SLOT_AOT_FRAME_OFFSET, base, offset, type_id};
    return slot;
}

static inline XrSlotRef xr_slot_jit_suspend(void *base, uint32_t offset, uint16_t type_id) {
    XrSlotRef slot = {XR_SLOT_JIT_SUSPEND, base, offset, type_id};
    return slot;
}

XR_FUNC bool xr_slot_store_value(XrSlotRef slot, XrValue value);
XR_FUNC XrValue *xr_slot_value_address(XrSlotRef slot);

XR_FUNC XrCoroBlockResult xr_coro_chan_send_resume(struct XrCoroutine *coro, XrSlotRef result_slot);
XR_FUNC XrCoroBlockResult xr_coro_chan_recv_resume(struct XrayIsolate *isolate,
                                                   struct XrCoroutine *coro, XrSlotRef value_slot,
                                                   XrSlotRef ok_slot);

XR_FUNC XrCoroBlockResult xr_coro_chan_send(struct XrayIsolate *isolate, struct XrCoroutine *coro,
                                            XrChannel *ch, XrValue value, XrSlotRef result_slot,
                                            int64_t timeout_ms);
XR_FUNC XrCoroBlockResult xr_coro_chan_recv(struct XrayIsolate *isolate, struct XrCoroutine *coro,
                                            XrChannel *ch, XrSlotRef value_slot, XrSlotRef ok_slot,
                                            int64_t timeout_ms);

#endif  // XBLOCK_H
