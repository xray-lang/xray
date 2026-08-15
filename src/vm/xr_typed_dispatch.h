/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_dispatch.h - Typed TargetPlan scalar executor
 */

#ifndef XR_TYPED_DISPATCH_H
#define XR_TYPED_DISPATCH_H

#include "../plan/target/xr_target_plan.h"
#include "debug/xr_vm_trace.h"
#include "xr_vm_dynamic_entry.h"

typedef struct XrVmDecodedCache XrVmDecodedCache;

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
    XR_TYPED_DISPATCH_TRACE_REJECTED,
    XR_TYPED_DISPATCH_ENTRY_UNAVAILABLE,
    XR_TYPED_DISPATCH_ENTRY_AUTHORITY_MISMATCH,
    XR_TYPED_DISPATCH_ENTRY_BUDGET_EXCEEDED,
    XR_TYPED_DISPATCH_ENTRY_RELEASE_FAILED,
} XrTypedDispatchStatus;

/*
 * Executed rows across one complete VM-to-VM call tree. It is a fixed budget
 * rather than a wall clock so the limit is a property of the program and the
 * same entry call refused once is refused every time.
 */
#define XR_TYPED_DISPATCH_MAX_STEPS UINT32_C(1048576)

/* Recursive direct-local execution shares one bounded native call stack. */
#define XR_TYPED_DISPATCH_MAX_CALL_DEPTH UINT32_C(256)

/* Arguments are positional signed i64 values. The count must equal the
 * parameter count the verified instruction group binds; a shorter, longer, or
 * absent vector is rejected rather than truncated or zero filled. The request
 * is the one execution boundary: optional runtime services extend it without
 * growing a positional ABI or introducing alternate entry points. Provider is
 * mandatory: callers choose one generated provider explicitly, and zero or an
 * unknown value is rejected rather than selecting a default implementation. */
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

/* Proves that one provider's generated binding exactly matches the canonical
 * opcode contract. It grants no authority to verify or execute a plan. */
XR_FUNC bool xr_typed_dispatch_provider_contract_is_exact(
    XrTypedDispatchProvider provider, uint16_t opcode,
    const XrTargetInstructionContract *contract);
XR_FUNC XrTypedDispatchStatus xr_typed_dispatch_execute_i64(
    const XrTypedDispatchI64Request *request);

#endif  // XR_TYPED_DISPATCH_H
