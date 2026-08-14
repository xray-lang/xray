/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_frame.h - Verified TargetPlan slot arena boundary
 *
 * KEY CONCEPT:
 *   A frame owns only packed bytes and side metadata. Every access repeats the
 *   exact immutable slot identity and representation contract from its plan.
 */

#ifndef XR_TYPED_FRAME_H
#define XR_TYPED_FRAME_H

#include "../plan/target/xr_target_plan.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_TYPED_FRAME_MAX_ARENA_BYTES ((size_t) 64u * 1024u * 1024u)
#define XR_TYPED_FRAME_MAX_SLOT_COUNT UINT32_C(1048576)
#define XR_TYPED_FRAME_MAX_ALIGNMENT ((size_t) 4096u)
#define XR_TYPED_FRAME_MAX_TOTAL_BYTES ((size_t) 80u * 1024u * 1024u)
#define XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION UINT32_C(25)
/* The exact closure the production builder completes, named once rather than
 * copied. A second hand-kept list of the same families is what let this
 * boundary fall a family behind and silently reject every plan the builder
 * emits, which reads as a frame failure rather than as a missing family. */
#define XR_TYPED_FRAME_SUPPORTED_FAMILY_MASK XR_TARGET_REQUIRED_FAMILIES

typedef enum XrTypedFrameStatus {
    XR_TYPED_FRAME_OK = 0,
    XR_TYPED_FRAME_INVALID_ARGUMENT,
    XR_TYPED_FRAME_PLAN_NOT_VERIFIED,
    XR_TYPED_FRAME_PLAN_IDENTITY_MISMATCH,
    XR_TYPED_FRAME_UNSUPPORTED_FAMILY,
    XR_TYPED_FRAME_FUNCTION_INVALID,
    XR_TYPED_FRAME_SLOT_INVALID,
    XR_TYPED_FRAME_ACCESS_MISMATCH,
    XR_TYPED_FRAME_BUDGET_EXHAUSTED,
    XR_TYPED_FRAME_ALLOCATION_FAILED,
    XR_TYPED_FRAME_UNINITIALIZED,
    XR_TYPED_FRAME_POISONED,
    XR_TYPED_FRAME_CLEANED,
} XrTypedFrameStatus;

typedef enum XrTypedSlotState {
    XR_TYPED_SLOT_STATE_INVALID = 0,
    XR_TYPED_SLOT_STATE_UNINITIALIZED,
    XR_TYPED_SLOT_STATE_INITIALIZED,
    XR_TYPED_SLOT_STATE_POISONED,
} XrTypedSlotState;

typedef struct XrTypedFrameLimits {
    size_t max_arena_bytes;
    uint32_t max_slot_count;
    size_t max_total_bytes;
} XrTypedFrameLimits;

typedef struct XrTypedSlotAccess {
    XrStableId identity;
    uint32_t slot;
    uint32_t size;
    uint16_t alignment;
    uint16_t register_rep;
    uint16_t memory_rep;
    uint16_t reserved;
} XrTypedSlotAccess;

typedef struct XrTypedFrame XrTypedFrame;

XR_FUNC void xr_typed_frame_limits_default(XrTypedFrameLimits *limits);
XR_FUNC XrTypedFrameStatus xr_typed_frame_create(
    const XrTargetPlan *verified_plan,
    const XrFingerprint *required_plan_fingerprint, uint32_t function,
    const XrTypedFrameLimits *limits, XrTypedFrame **frame);
XR_FUNC XrTypedFrameStatus xr_typed_frame_describe_slot(
    const XrTypedFrame *frame, uint32_t slot, XrTypedSlotAccess *access);
XR_FUNC XrTypedFrameStatus xr_typed_frame_store(
    XrTypedFrame *frame, const XrTypedSlotAccess *access, const void *bytes,
    size_t size);
XR_FUNC XrTypedFrameStatus xr_typed_frame_load(
    const XrTypedFrame *frame, const XrTypedSlotAccess *access, void *bytes,
    size_t size);
XR_FUNC XrTypedFrameStatus xr_typed_frame_poison(
    XrTypedFrame *frame, const XrTypedSlotAccess *access);
XR_FUNC XrTypedFrameStatus xr_typed_frame_slot_state(
    const XrTypedFrame *frame, uint32_t slot, XrTypedSlotState *state);
XR_FUNC size_t xr_typed_frame_arena_size(const XrTypedFrame *frame);
XR_FUNC uint32_t xr_typed_frame_slot_count(const XrTypedFrame *frame);
XR_FUNC XrTypedFrameStatus xr_typed_frame_cleanup(XrTypedFrame *frame);
XR_FUNC void xr_typed_frame_free(XrTypedFrame *frame);

#endif  // XR_TYPED_FRAME_H
