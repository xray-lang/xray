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
 *   The standard runtime module reads counters through five exact scalar
 *   leaves. Each leaf is a plain read of the current execution-local
 *   reclamation domain, so no host object shape, no ownership transfer and no
 *   suspension crosses the declaration boundary. Names, signatures and
 *   ownership live in the generated declaration table; this file records only
 *   which identities the runtime module may declare.
 */
#ifndef XXIR_RUNTIME_NATIVE_H
#define XXIR_RUNTIME_NATIVE_H
#include <string.h>
#include "xxir_types.h"
#include "../shared/xnative_declaration.h"

static const uint32_t xir_runtime_native_ids[] = {
    XR_NATIVE_CALLABLE_RUNTIME_LIVE_BYTES,
    XR_NATIVE_CALLABLE_RUNTIME_LIVE_OBJECTS,
    XR_NATIVE_CALLABLE_RUNTIME_FINALIZER_COUNT,
    XR_NATIVE_CALLABLE_RUNTIME_SHARED_LIVE_BYTES,
    XR_NATIVE_CALLABLE_RUNTIME_STATIC_ALLOC_BYTES
};

#define XIR_RUNTIME_NATIVE_COUNT (sizeof(xir_runtime_native_ids) / sizeof(*xir_runtime_native_ids))

static inline bool xir_runtime_native_id(uint32_t id) {
    for (uint32_t n = 0; n < XIR_RUNTIME_NATIVE_COUNT; ++n)
        if (xir_runtime_native_ids[n] == id) return true;
    return false;
}

/* Every counter has the same shape: an exact zero-argument read that returns
 * one i64 and cannot fail. Anything else is a different contract and is
 * rejected rather than coerced. The name comes from the generated declaration
 * table, so a renamed leaf is a generator change and not a second copy here. */
static inline bool xir_runtime_native_signature(uint32_t id, const XrXirTypes *types,
    const XrXirType *parameters, uint32_t count, XrXirType result, XrXirType error) {
    (void) types;
    if (!xir_runtime_native_id(id)) return false;
    const XrNativeCallableDeclaration *native = xr_native_callable_by_id(id);
    if (!native || native->owner || native->error_e_type || native->ctfe ||
        !native->namespace_id || strcmp(native->namespace_id, "runtime") ||
        !native->source_path || strcmp(native->source_path, "stdlib/defs/core.def") ||
        !native->member) return false;
    const XrNativeMemberDeclaration *member = native->member;
    return member->name && member->name[0] &&
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
