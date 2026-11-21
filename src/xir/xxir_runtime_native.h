/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_runtime_native.h - Governed execution-domain counters for the runtime module
 *
 * KEY CONCEPT:
 *   The standard runtime module reads counters through three exact scalar
 *   leaves. Each leaf is a plain read of the current execution-local
 *   reclamation domain, so no host object shape, no ownership transfer and no
 *   suspension crosses the declaration boundary.
 */
#ifndef XXIR_RUNTIME_NATIVE_H
#define XXIR_RUNTIME_NATIVE_H
#include <string.h>
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"

typedef struct XirRuntimeNativeContract {
    uint32_t id;
    const char *name;
} XirRuntimeNativeContract;

static const XirRuntimeNativeContract xir_runtime_native_contracts[] = {
    {XR_NATIVE_CALLABLE_RUNTIME_LIVE_BYTES, "__liveBytes"},
    {XR_NATIVE_CALLABLE_RUNTIME_LIVE_OBJECTS, "__liveObjects"},
    {XR_NATIVE_CALLABLE_RUNTIME_FINALIZER_COUNT, "__finalizerCount"},
    {XR_NATIVE_CALLABLE_RUNTIME_SHARED_LIVE_BYTES, "__sharedLiveBytes"},
    {XR_NATIVE_CALLABLE_RUNTIME_STATIC_ALLOC_BYTES, "__staticAllocBytes"}
};

static inline const XirRuntimeNativeContract *xir_runtime_native_contract(uint32_t id) {
    for (uint32_t n = 0; n < sizeof(xir_runtime_native_contracts) / sizeof(*xir_runtime_native_contracts); ++n)
        if (xir_runtime_native_contracts[n].id == id) return xir_runtime_native_contracts + n;
    return NULL;
}

static inline bool xir_runtime_native_id(uint32_t id) {
    return xir_runtime_native_contract(id) != NULL;
}

/* Every counter has the same shape: an exact zero-argument read that returns
 * one i64 and cannot fail. Anything else is a different contract and is
 * rejected rather than coerced. */
static inline bool xir_runtime_native_signature(uint32_t id, const XrXirTypes *types,
    const XrXirType *parameters, uint32_t count, XrXirType result, XrXirType error) {
    (void) types;
    const XirRuntimeNativeContract *contract = xir_runtime_native_contract(id);
    const XrNativeCallableDeclaration *native = xr_native_callable_by_id(id);
    if (!contract || !native || native->owner || native->error_e_type || native->ctfe ||
        !native->namespace_id || strcmp(native->namespace_id, "runtime") ||
        !native->source_path || strcmp(native->source_path, "stdlib/defs/core.def") ||
        !native->member) return false;
    const XrNativeMemberDeclaration *member = native->member;
    return member->name && !strcmp(member->name, contract->name) &&
        member->signature && !strcmp(member->signature, "(): i64") &&
        member->result_text && !strcmp(member->result_text, "i64") &&
        !member->is_public && !member->is_static && !member->is_method && !member->lowered &&
        member->receiver == XR_NATIVE_RECEIVER_READ &&
        member->operation == XR_NATIVE_OPERATION_NONE &&
        member->allocation == XR_NATIVE_ALLOCATION_NO_HEAP &&
        member->ownership == XR_NATIVE_RESULT_OWNERSHIP_OWNED &&
        member->result == XR_NATIVE_TERM_I64 && member->parameter_count == 0 &&
        count == 0 && !parameters && result == XR_XIR_I64 && error == XR_XIR_NEVER;
}
#endif // XXIR_RUNTIME_NATIVE_H
