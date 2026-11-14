/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_value.h - One execution owner for native managed values
 *
 * KEY CONCEPT:
 *   The caller owns this mutable state across the synchronous execution's
 *   setjmp boundary. Normal return and failure drain the same physical owners.
 */
#ifndef XNATIVE_VALUE_H
#define XNATIVE_VALUE_H
#include "xray_native_runtime.h"
#include "xir/xxir_value.h"

typedef struct XrtStringOwner XrtStringOwner;
typedef struct XrtStringLiteral XrtStringLiteral;
typedef struct XrtArrayOwner XrtArrayOwner;
typedef struct XrtMapOwner XrtMapOwner;
typedef struct XrtViewRegistry XrtViewRegistry;
typedef struct XrtCallableRegistry XrtCallableRegistry;
typedef struct XrtCallableScope XrtCallableScope;
typedef struct XrtBoxedOwner XrtBoxedOwner;
typedef struct XrtBoxedCatalog XrtBoxedCatalog;

typedef struct XrtValueRoot {
    XrXirDomain *domain;
    XrtStringOwner *table;
    XrtStringLiteral *literals;
    uint64_t held;
    uint32_t table_capacity, table_count, literal_capacity, literal_count;
    uint64_t byte_limit; // Zero selects the ordinary unbounded runtime policy.
    XrXirTypeArena *types;
    XrtArrayOwner *arrays;
    uint32_t array_capacity, array_count;
    uint64_t array_held;
    XrXirTypeArena *map_types;
    XrtMapOwner *maps;
    uint32_t map_capacity, map_count;
    uint64_t map_held;
    XrXirValue pending;
    XrtViewRegistry *views;
    XrtCallableRegistry *callables;
    XrtCallableScope *callable_scope;
    uint64_t callable_identity;
    XrtBoxedOwner *boxed;
    XrtBoxedCatalog *boxed_catalogs;
    void *boxed_scratch;
    size_t boxed_scratch_size;
} XrtValueRoot;

typedef struct XrtValueBinding {
    XrtValueRoot *root;
    size_t sources;
} XrtValueBinding;

/* Binding borrows this thread's active execution and source-map extent.
 * The returned previous binding is restored on every execution exit. */
XR_FUNC XrtValueBinding xrt_value_root_bind(XrtValueBinding binding);
XR_FUNC XR_NORET void xrt_value_failure(XrXirValueStatus status, uint32_t source_id);
XR_FUNC void xrt_value_root_drain(XrtValueRoot *root);
XR_FUNC XrtValueRoot *xrt_value_root_current(uint32_t source_id);
XR_FUNC XrtValueRoot *xrt_value_root_active(void);
XR_FUNC XrXirDomain *xrt_value_root_domain(XrtValueRoot *, uint32_t source_id);
static inline bool xrt_value_root_empty(const XrtValueRoot *root) {
    return !root->domain && !root->table && !root->literals && !root->held &&
        !root->table_capacity && !root->table_count && !root->literal_capacity && !root->literal_count &&
        !root->types && !root->arrays && !root->array_capacity && !root->array_count &&
        !root->array_held && !root->map_types && !root->maps && !root->map_capacity &&
        !root->map_count && !root->map_held && !root->views && !root->callables && !root->callable_scope &&
        !root->pending.type && !root->pending.reserved && !root->pending.payload &&
        !root->boxed && !root->boxed_catalogs && !root->boxed_scratch && !root->boxed_scratch_size;
}
#endif // XNATIVE_VALUE_H
