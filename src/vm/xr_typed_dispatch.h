/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_dispatch.h - Typed TargetPlan executor
 */

#ifndef XR_TYPED_DISPATCH_H
#define XR_TYPED_DISPATCH_H

#include "../plan/target/xr_target_plan.h"
#include "../runtime/value/xvalue.h"
#include "../shared/xr_array_push_status.h"
#include "debug/xr_vm_trace.h"
#include "xr_vm_dynamic_entry.h"

typedef struct XrVmDecodedCache XrVmDecodedCache;
typedef struct XrTypedCoroutineI64 XrTypedCoroutineI64;
typedef XrArrayPushStatus (*XrTypedArrayPushKernel)(XrValue receiver, XrValue value);

/* The first executable leaf-value aggregate family is exactly two target i64
 * fields in declaration order.  This carrier is only the request boundary;
 * the verified TargetPlan remains the authority for its 16-byte layout,
 * representation, slots, field offsets, and direct-call storage. */
typedef struct XrTypedLeafAggregateI64x2 {
    int64_t fields[2];
} XrTypedLeafAggregateI64x2;

/* Exact pointer-free tuple6 carrier.  Padding is part of the x64 ABI layout
 * proved by schema-54 TargetPlan rows; callers never expose plan slots. */
typedef struct XrTypedLeafValueProductTuple6 {
    int64_t field0;
    int64_t field1;
    uint8_t field2;
    uint8_t reserved2[7];
    int64_t field3;
    int64_t field4;
    int64_t field5;
} XrTypedLeafValueProductTuple6;

typedef enum XrTypedDispatchProvider {
    XR_TYPED_DISPATCH_PROVIDER_INVALID = 0,
    XR_TYPED_DISPATCH_PROVIDER_GENERATED_SWITCH,
    XR_TYPED_DISPATCH_PROVIDER_GENERATED_FUNCTION_TABLE,
    XR_TYPED_DISPATCH_PROVIDER_COUNT,
} XrTypedDispatchProvider;

typedef enum XrTypedDispatchStatus {
    XR_TYPED_DISPATCH_OK = 0,
    XR_TYPED_DISPATCH_INVALID_ARGUMENT,
    XR_TYPED_DISPATCH_PLAN_NOT_VERIFIED,
    XR_TYPED_DISPATCH_PLAN_IDENTITY_MISMATCH,
    XR_TYPED_DISPATCH_PROGRAM_UNAVAILABLE,
    XR_TYPED_DISPATCH_PROGRAM_INVALID,
    XR_TYPED_DISPATCH_ARGUMENT_MISMATCH,
    XR_TYPED_DISPATCH_FRAME_ERROR,
    /* The executed program's own error edge, kept apart from every status
     * above: those report that the plan or the call was unacceptable, while
     * these report that an acceptable program divided by zero. They stay
     * distinct from each other so a caller can raise the exact panic the
     * language names for each operator. */
    XR_TYPED_DISPATCH_DIVIDE_BY_ZERO,
    XR_TYPED_DISPATCH_MODULO_BY_ZERO,
    /* A verified program may loop forever: no static proof forbids it, and one
     * that did would refuse ordinary loops. The executor bounds the work
     * instead and stops with this status, so a plan can never hang the caller
     * that ran it. */
    XR_TYPED_DISPATCH_STEP_LIMIT_EXCEEDED,
    XR_TYPED_DISPATCH_CALL_DEPTH_EXCEEDED,
    XR_TYPED_DISPATCH_DEBUG_IDENTITY_MISMATCH,
    XR_TYPED_DISPATCH_DEBUG_CONTROL_ERROR,
    XR_TYPED_DISPATCH_DEBUG_TERMINATED,
    XR_TYPED_DISPATCH_DEBUG_STOP_REJECTED,
    XR_TYPED_DISPATCH_TRACE_REJECTED,
    XR_TYPED_DISPATCH_ENTRY_UNAVAILABLE,
    XR_TYPED_DISPATCH_ENTRY_AUTHORITY_MISMATCH,
    XR_TYPED_DISPATCH_ENTRY_BUDGET_EXCEEDED,
    XR_TYPED_DISPATCH_ENTRY_NATIVE_ERROR,
    XR_TYPED_DISPATCH_ENTRY_CANCELLED,
    XR_TYPED_DISPATCH_ENTRY_RETIRE_DEFERRED,
    XR_TYPED_DISPATCH_SUSPENDED,
    XR_TYPED_DISPATCH_ARRAY_PUSH_INVALID_RECEIVER,
    XR_TYPED_DISPATCH_ARRAY_PUSH_SLICE,
    XR_TYPED_DISPATCH_ARRAY_PUSH_TYPE_MISMATCH,
    XR_TYPED_DISPATCH_ARRAY_PUSH_ALLOCATION_FAILED,
} XrTypedDispatchStatus;

/*
 * Executed rows across one complete VM-to-VM call tree. It is a fixed budget
 * rather than a wall clock so the limit is a property of the program and the
 * same entry call refused once is refused every time.
 */
#define XR_TYPED_DISPATCH_MAX_STEPS UINT32_C(1048576)

/* Recursive direct-local execution shares one bounded native call stack. */
#define XR_TYPED_DISPATCH_MAX_CALL_DEPTH UINT32_C(128)

/* Arguments are positional signed i64 values. The count must equal the
 * parameter count the verified instruction group binds; a shorter, longer, or
 * absent vector is rejected rather than truncated or zero filled. For a
 * program graph, function must be its verified entry target row; producer rows
 * are reachable only through same-plan direct calls. The request is the one
 * execution boundary: optional runtime services extend it without growing a
 * positional ABI or introducing alternate entry points. Provider is mandatory:
 * callers choose one generated provider explicitly, and zero or an unknown
 * value is rejected rather than selecting a default implementation. */
typedef struct XrTypedDispatchI64Request {
    const XrTargetPlan *verified_plan;
    const XrFingerprint *required_plan_fingerprint;
    const int64_t *arguments;
    int64_t *result;
    const XrVmDebugSession *debug_session;
    const XrVmDecodedCache *decoded_cache;
    const XrVmDynamicEntryContext *dynamic_entries;
    const XrModuleGenerationIdentity *generation_identity;
    XrTypedDispatchProvider provider;
    uint32_t function;
    uint32_t argument_count;
    bool use_dynamic_entry_cache;
} XrTypedDispatchI64Request;

/* Managed XrValue entry for the exact tagged Array.push instruction family.
 * `arguments` is mutable because argument 1 transfers ownership: the executor
 * clears it once the owner enters the frame, keeps it clear after success, and
 * restores the exact value on every failure.  Argument 0 is borrowed and is
 * never modified. */
typedef struct XrTypedDispatchValueRequest {
    const XrTargetPlan *verified_plan;
    const XrFingerprint *required_plan_fingerprint;
    XrValue *arguments;
    XrTypedArrayPushKernel array_push;
    XrTypedDispatchProvider provider;
    uint32_t function;
    uint32_t argument_count;
} XrTypedDispatchValueRequest;

/* Pointer-free entry for the exact leaf aggregate i64x2 execution family.
 * Arguments are positional complete values.  A zero-parameter root therefore
 * passes NULL with argument_count zero; a unary function passes one complete
 * value.  Neither side may supply or observe a TargetPlan slot directly. */
typedef struct XrTypedDispatchLeafAggregateI64x2Request {
    const XrTargetPlan *verified_plan;
    const XrFingerprint *required_plan_fingerprint;
    const XrTypedLeafAggregateI64x2 *arguments;
    XrTypedLeafAggregateI64x2 *result;
    XrTypedDispatchProvider provider;
    uint32_t function;
    uint32_t argument_count;
} XrTypedDispatchLeafAggregateI64x2Request;

typedef struct XrTypedDispatchLeafValueProductTuple6Request {
    const XrTargetPlan *verified_plan;
    const XrFingerprint *required_plan_fingerprint;
    XrTypedLeafValueProductTuple6 *result;
    XrTypedDispatchProvider provider;
    uint32_t function;
} XrTypedDispatchLeafValueProductTuple6Request;

/* A suspended typed coroutine is single-owner and owns one packed frame and
 * one immutable decoded program. Resuming reuses those exact objects; no
 * retained legacy value stack or bytecode frame is created. The initial slice
 * is deliberately closed to zero-managed-lifecycle scalar i64 programs and
 * rejects debug, dynamic-entry, and generation services until their suspension
 * ownership protocols exist.
 * No API below may be called concurrently for the same coroutine object. */
typedef struct XrTypedCoroutineI64Request {
    const XrTargetPlan *verified_plan;
    const XrFingerprint *required_plan_fingerprint;
    const int64_t *arguments;
    XrTypedDispatchProvider provider;
    uint32_t function;
    uint32_t argument_count;
} XrTypedCoroutineI64Request;

/* Proves that one provider's generated binding exactly matches the canonical
 * opcode contract. It grants no authority to verify or execute a plan. */
XR_FUNC bool xr_typed_dispatch_provider_contract_is_exact(
    XrTypedDispatchProvider provider, uint16_t opcode,
    const XrTargetInstructionContract *contract);
XR_FUNC XrTypedDispatchStatus xr_typed_dispatch_execute_i64(
    const XrTypedDispatchI64Request *request);
XR_FUNC XrTypedDispatchStatus xr_typed_dispatch_execute_values(
    const XrTypedDispatchValueRequest *request);
XR_FUNC XrTypedDispatchStatus xr_typed_dispatch_execute_leaf_aggregate_i64x2(
    const XrTypedDispatchLeafAggregateI64x2Request *request);
XR_FUNC XrTypedDispatchStatus xr_typed_dispatch_execute_leaf_value_product_tuple6(
    const XrTypedDispatchLeafValueProductTuple6Request *request);
/* Creation requires an empty owning output slot. Failure preserves that slot;
 * success transfers the sole coroutine owner into it. Free is the only API
 * that releases the owned frame/cache/plan and clears the slot. */
XR_FUNC XrTypedDispatchStatus xr_typed_coroutine_i64_create(
    const XrTypedCoroutineI64Request *request,
    XrTypedCoroutineI64 **coroutine);
XR_FUNC XrTypedDispatchStatus xr_typed_coroutine_i64_resume(
    XrTypedCoroutineI64 *coroutine, int64_t *result,
    uint32_t *suspended_state);
XR_FUNC XrTypedDispatchStatus xr_typed_coroutine_i64_cancel(
    XrTypedCoroutineI64 *coroutine);
XR_FUNC XrTypedDispatchStatus xr_typed_coroutine_i64_free(
    XrTypedCoroutineI64 **coroutine);

#endif  // XR_TYPED_DISPATCH_H
