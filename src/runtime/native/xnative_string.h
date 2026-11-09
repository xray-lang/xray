/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_string.h - Physical owners of canonical String references
 *
 * KEY CONCEPT:
 *   A root holds one real reference per String its guest owns and counts the
 *   guest references itself, so ending the root releases exactly those.
 */
#ifndef XNATIVE_STRING_H
#define XNATIVE_STRING_H
#include "xray_native_runtime.h"
#include "xir/xxir_value.h"

typedef struct XrtStringOwner XrtStringOwner;
typedef struct XrtStringLiteral XrtStringLiteral;
typedef struct XrtStringRoot {
    XrXirDomain *domain;
    XrtStringOwner *table;       // Open-addressed owned Strings; NULL until first use.
    XrtStringLiteral *literals;  // Literal objects pinned until the root ends.
    uint64_t held;               // Guest references currently owned in this root.
    uint32_t table_capacity, table_count, literal_capacity, literal_count;
    uint64_t byte_limit; // Zero selects the ordinary unbounded runtime policy.
} XrtStringRoot;

/* The String root of this thread's active execution and the size of its
 * source map. Binding returns the previous pair, which the execution restores
 * on every exit; String operations outside a bound root abort. */
typedef struct XrtStringBinding {
    XrtStringRoot *root;
    size_t sources;
} XrtStringBinding;
XR_FUNC XrtStringBinding xrt_string_root_bind(XrtStringBinding binding);
XR_FUNC XR_NORET void xrt_value_failure(XrXirValueStatus status, uint32_t source_id);
XR_FUNC void xrt_string_root_drain(XrtStringRoot *root);
static inline bool xrt_string_root_empty(const XrtStringRoot *root) {
    return !root->domain && !root->table && !root->literals && !root->held &&
        !root->table_capacity && !root->table_count && !root->literal_capacity && !root->literal_count;
}
/* The authenticated producer transfers one current root owner before close.
 * No retained handle into the root bookkeeping is exposed to its receiver. */
XR_FUNC XrtString *xrt_string_root_take(XrtString *string);
#endif // XNATIVE_STRING_H
