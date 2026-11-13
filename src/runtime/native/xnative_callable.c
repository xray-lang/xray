/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_callable.c - Canonical captures retain their precise executable owner
 *
 * KEY CONCEPT:
 *   One immutable receiving directory owns every capture shape. Canonical Fn
 *   destruction releases its code lease after releasing capture ownership
 *   edges. Queued child objects can finish retirement afterward.
 */
#include "xnative_callable.h"
#include "xir/xxir_types.h"
#include "base/xmalloc.h"
#include "base/xchecks.h"
#include <stdatomic.h>
#include <string.h>

struct XrtCallableCode {
    _Atomic(uint32_t) references;
    XrXirTypeArena *arena;
    XrtCallableLease lease;
    uint32_t count;
    XrtCallableEntry entries[];
};

static void callable_code_release(void *owner) {
    XrtCallableCode *code = owner;
    if (atomic_fetch_sub_explicit(&code->references, 1, memory_order_acq_rel) != 1) return;
    XrtCallableLease lease = code->lease;
    XrXirTypeArena *arena = code->arena;
    xr_free(code);
    lease.release(lease.owner);
    xr_xir_compile_type_arena_drop(arena);
}

XR_FUNC bool xrt_callable_code_retain(XrtCallableCode *code) {
    if (!code) return false;
    uint32_t count = atomic_load_explicit(&code->references, memory_order_relaxed);
    while (count && count != UINT32_MAX) {
        if (atomic_compare_exchange_weak_explicit(&code->references, &count, count + 1,
                memory_order_relaxed, memory_order_relaxed)) return true;
    }
    return false;
}

XR_FUNC void xrt_callable_code_drop(XrtCallableCode **owner) {
    if (!owner || !*owner) return;
    XrtCallableCode *code = *owner;
    *owner = NULL;
    callable_code_release(code);
}

XR_FUNC XrXirValueStatus xrt_callable_code_new(XrXirTypeArena *arena,
    const XrtCallableEntry *entries, uint32_t count, XrtCallableLease lease, XrtCallableCode **empty) {
    if (!arena || !count || !entries || !empty || *empty ||
        !lease.owner || !lease.retain || !lease.release) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
    size_t captures = 0;
    for (uint32_t e = 0; e < count; ++e) {
        XrtCallableEntry entry = entries[e];
        const XrXirTypeNode *signature = xr_xir_callable_signature(types, entry.type);
        if (!signature || ((signature->flags & XR_XIR_CALLABLE_C_ABI) && entry.capture_count) ||
            entry.capture_count > 65536 ||
            (entry.capture_count && !entry.captures)) return XR_XIR_VALUE_BAD_ARGUMENT;
        if (entry.capture_count > SIZE_MAX - captures) return XR_XIR_VALUE_OVERFLOW;
        captures += entry.capture_count;
        for (uint32_t c = 0; c < entry.capture_count; ++c) {
            XrXirLayout layout;
            if (xr_xir_compile_type_arena_borrowed(arena, entry.captures[c]) ||
                !xr_xir_compile_type_arena_layout(arena, entry.captures[c], &layout))
                return XR_XIR_VALUE_BAD_ARGUMENT;
        }
    }
    size_t entries_bytes = count;
    if (entries_bytes > (SIZE_MAX - sizeof(XrtCallableCode)) / sizeof(XrtCallableEntry))
        return XR_XIR_VALUE_OVERFLOW;
    size_t bytes = sizeof(XrtCallableCode) + entries_bytes * sizeof(XrtCallableEntry);
    if (captures > (SIZE_MAX - bytes) / sizeof(XrXirType)) return XR_XIR_VALUE_OVERFLOW;
    bytes += captures * sizeof(XrXirType);
    XrtCallableCode *code = xr_malloc(bytes);
    if (!code) return XR_XIR_VALUE_OOM;
    if (!xr_xir_compile_type_arena_retain(arena)) { xr_free(code); return XR_XIR_VALUE_REFCOUNT_LIMIT; }
    if (!lease.retain(lease.owner)) {
        xr_xir_compile_type_arena_drop(arena);
        xr_free(code);
        return XR_XIR_VALUE_REFCOUNT_LIMIT;
    }
    atomic_init(&code->references, 1);
    code->arena = arena;
    code->lease = lease;
    code->count = count;
    _Static_assert(sizeof(XrtCallableEntry) % _Alignof(XrXirType) == 0,
        "entry tails preserve capture type alignment");
    XrXirType *next = (XrXirType *)(code->entries + count);
    for (uint32_t e = 0; e < count; ++e) {
        code->entries[e] = entries[e];
        code->entries[e].captures = entries[e].capture_count ? next : NULL;
        if (entries[e].capture_count) {
            memcpy(next, entries[e].captures, (size_t)entries[e].capture_count * sizeof(*next));
            next += entries[e].capture_count;
        }
    }
    *empty = code;
    return XR_XIR_VALUE_OK;
}

static XrXirValueStatus callable_admit(void *owner, const XrXirFunctionBinding *binding,
    XrXirType type, uint64_t *work) {
    (void)work;
    const XrtCallableCode *code = owner;
    if (!binding || binding->owner != code || binding->release != callable_code_release ||
        binding->entry >= code->count) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrtCallableEntry entry = code->entries[binding->entry];
    if (type != entry.type || binding->capture_count != entry.capture_count ||
        (entry.capture_count && !binding->captures)) return XR_XIR_VALUE_BAD_ARGUMENT;
    for (uint32_t c = 0; c < entry.capture_count; ++c)
        if (!xr_xir_value_argument(&binding->captures[c], code->arena, entry.captures[c]))
            return XR_XIR_VALUE_BAD_ARGUMENT;
    return XR_XIR_VALUE_OK;
}

XR_FUNC XrXirValueStatus xrt_callable_new(XrtCallableCode *code, XrXirDomain *domain,
    uint32_t entry, const XrXirValue *captures, uint32_t count, XrXirValue *empty) {
    if (!code || entry >= code->count || !domain || !empty || empty->type || empty->reserved || empty->payload)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrtCallableEntry shape = code->entries[entry];
    XrXirFunctionBinding binding = {code, callable_code_release, entry, captures, count};
    XrXirValueStatus status = callable_admit(code, &binding, shape.type, NULL);
    if (status != XR_XIR_VALUE_OK) return status;
    if (!xrt_callable_code_retain(code)) return XR_XIR_VALUE_REFCOUNT_LIMIT;
    XrXirValueAdmission admission = {code->arena, domain, callable_admit, code, UINT64_MAX, UINT64_MAX};
    status = xr_xir_function_new(domain, code->arena, shape.type, &binding, &admission, empty);
    if (status != XR_XIR_VALUE_OK) callable_code_release(code);
    return status;
}

XR_FUNC XrXirValueStatus xrt_callable_borrow(const XrtCallableCode *code, const XrXirValue *value,
    uint32_t entry, const XrXirValue **captures, uint32_t *count) {
    if (!code || entry >= code->count || !captures || !count ||
        !xr_xir_value_argument(value, code->arena, code->entries[entry].type)) return XR_XIR_VALUE_BAD_ARGUMENT;
    const XrXirFunctionBinding *binding = xr_xir_function_binding(value);
    if (!binding || binding->entry != entry || callable_admit((void *)code, binding,
            code->entries[entry].type, NULL) != XR_XIR_VALUE_OK) return XR_XIR_VALUE_BAD_ARGUMENT;
    *captures = binding->captures;
    *count = binding->capture_count;
    return XR_XIR_VALUE_OK;
}
