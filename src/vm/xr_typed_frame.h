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

#include "../../include/xray_runtime_generation.h"
#include "../plan/target/xr_target_plan.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_TYPED_FRAME_MAX_ARENA_BYTES ((size_t) 64u * 1024u * 1024u)
#define XR_TYPED_FRAME_MAX_SLOT_COUNT UINT32_C(1048576)
#define XR_TYPED_FRAME_MAX_ALIGNMENT ((size_t) 4096u)
#define XR_TYPED_FRAME_MAX_TOTAL_BYTES ((size_t) 80u * 1024u * 1024u)
#define XR_TYPED_FRAME_SUPPORTED_PLAN_SCHEMA_VERSION UINT32_C(42)
#define XR_TYPED_FRAME_CONTEXT_INDEX_NONE UINT32_MAX
#define XR_TYPED_FRAME_MAX_PARENT_DEPTH UINT32_C(4096)

/* Slot state is diagnostic metadata, never an execution tag. Release builds
 * omit it completely unless an audit target opts in explicitly; verified
 * definite assignment remains the release authority for every slot read. */
#if !defined(NDEBUG) || defined(XR_TYPED_FRAME_AUDIT_SLOT_STATES)
#define XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA 1
#else
#define XR_TYPED_FRAME_HAS_SLOT_STATE_METADATA 0
#endif
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
    XR_TYPED_FRAME_DEBUG_METADATA_UNAVAILABLE,
    XR_TYPED_FRAME_CONTEXT_UNAVAILABLE,
    XR_TYPED_FRAME_CONTEXT_TRANSITION_INVALID,
    XR_TYPED_FRAME_GENERATION_IDENTITY_MISMATCH,
    XR_TYPED_FRAME_CALL_LINK_INVALID,
    XR_TYPED_FRAME_CHILD_ACTIVE,
    XR_TYPED_FRAME_LIFECYCLE_INACTIVE,
    XR_TYPED_FRAME_LIFECYCLE_ACTIVE,
    XR_TYPED_FRAME_TERMINAL,
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

/* Load and store copy exactly one verified slot's complete object
 * representation. They never form a typed pointer to arena storage, infer a
 * representation from the caller's buffer, dereference carried references,
 * or perform retain/release. Rooted or owned storage is accepted only for the
 * exact frozen coroutine lifecycle below; every other nontrivial ownership
 * shape remains fail closed. */

/* Payload bytes requested from the allocator for one live frame. Allocator
 * bookkeeping and heap fragmentation are deliberately outside this contract:
 * neither is owned or knowable by the typed-frame implementation. */
typedef struct XrTypedFrameMemoryFootprint {
    size_t fixed_frame_bytes;
    size_t arena_allocation_bytes;
    size_t alignment_padding_bytes;
    size_t slot_state_metadata_bytes;
    size_t lifecycle_state_metadata_bytes;
    size_t total_bytes;
} XrTypedFrameMemoryFootprint;

/* A target function is identified by the immutable plan fingerprint plus the
 * exact target and semantic indexes. TargetPlan has no second name-based
 * function identity, so this tuple is the complete runtime identity. */
typedef struct XrTypedFunctionIdentity {
    XrFingerprint plan_fingerprint;
    uint32_t function;
    uint32_t semantic_function;
} XrTypedFunctionIdentity;

/* Read-only execution context. `block_entry_instruction` and `instruction`
 * are global TargetPlan instruction row IDs. A TargetPlan intentionally has no
 * target-level block table, so the first instruction row derived for the block
 * is its stable runtime identity. Generation fields copy the stable identity
 * supplied by the generation authority after checking every plan-bound field;
 * the caller remains responsible for the corresponding generation pin because
 * the public pin ABI does not expose an independently owned pin token. */
typedef struct XrTypedFrameContext {
    XrTypedFunctionIdentity function_identity;
    uint32_t block_entry_instruction;
    uint32_t instruction;
    uint32_t coroutine_state;
    uint32_t reserved;
    uint64_t generation_number;
    XrFingerprint generation_fingerprint;
    bool generation_bound;
    bool has_parent;
    bool has_child;
    bool terminal;
} XrTypedFrameContext;

typedef struct XrTypedFrame XrTypedFrame;

typedef void (*XrTypedFrameRootVisitor)(void *context,
                                        const XrTypedSlotAccess *access,
                                        const void *bytes);
typedef XrTypedFrameStatus (*XrTypedFrameCleanupExecutor)(
    void *context, uint8_t action, const XrTypedSlotAccess *access,
    void *bytes);

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
XR_FUNC XrTypedFrameStatus xr_typed_frame_context(
    const XrTypedFrame *frame, XrTypedFrameContext *context);
XR_FUNC XrTypedFrameStatus xr_typed_frame_enter_instruction(
    XrTypedFrame *frame, uint32_t instruction);
/* The dispatcher may use this O(1) entry only after it has bound the frame to
 * the same immutable plan as an exact verified decoded cache. */
XR_FUNC XrTypedFrameStatus xr_typed_frame_enter_decoded_instruction(
    XrTypedFrame *frame, uint32_t instruction,
    uint32_t block_entry_instruction);
XR_FUNC XrTypedFrameStatus xr_typed_frame_bind_coroutine_state(
    XrTypedFrame *frame, uint32_t coroutine_state);
XR_FUNC XrTypedFrameStatus xr_typed_frame_visit_coroutine_roots(
    XrTypedFrame *frame, uint32_t coroutine_state,
    XrTypedFrameRootVisitor visitor, void *context, uint32_t *visited);
XR_FUNC XrTypedFrameStatus xr_typed_frame_resume_coroutine_state(
    XrTypedFrame *frame, uint32_t coroutine_state);
XR_FUNC XrTypedFrameStatus xr_typed_frame_execute_cleanups(
    XrTypedFrame *frame, uint32_t semantic_operation, uint8_t event_flags,
    XrTypedFrameCleanupExecutor executor, void *context, uint32_t *executed);
XR_FUNC XrTypedFrameStatus xr_typed_frame_bind_generation_identity(
    XrTypedFrame *frame, const XrModuleGenerationIdentity *identity);
XR_FUNC XrTypedFrameStatus xr_typed_frame_link_child(
    XrTypedFrame *parent, XrTypedFrame *child);
XR_FUNC XrTypedFrameStatus xr_typed_frame_unlink_child(
    XrTypedFrame *parent, XrTypedFrame *child);
XR_FUNC size_t xr_typed_frame_arena_size(const XrTypedFrame *frame);
XR_FUNC uint32_t xr_typed_frame_slot_count(const XrTypedFrame *frame);
XR_FUNC XrTypedFrameStatus xr_typed_frame_memory_footprint(
    const XrTypedFrame *frame, XrTypedFrameMemoryFootprint *footprint);
XR_FUNC XrTypedFrameStatus xr_typed_frame_cleanup(XrTypedFrame *frame);
XR_FUNC XrTypedFrameStatus xr_typed_frame_free(XrTypedFrame **frame);

#endif  // XR_TYPED_FRAME_H
