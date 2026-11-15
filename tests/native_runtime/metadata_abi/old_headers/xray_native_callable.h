/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_callable.h - Typed canonical closures in synchronous scopes
 */
#ifndef XRAY_NATIVE_CALLABLE_H
#define XRAY_NATIVE_CALLABLE_H
#include "xray_native_pod.h"

typedef enum XrtCallableValueKind {
    XRT_CALLABLE_SCALAR, XRT_CALLABLE_STRING, XRT_CALLABLE_ARRAY, XRT_CALLABLE_CONSTRUCTED,
    XRT_CALLABLE_MAP_STRING_I64
} XrtCallableValueKind;
typedef struct XrtCallableValueSpec { XrtCallableValueKind kind; uint32_t type; } XrtCallableValueSpec;
typedef enum XrtCallableShapeKind {
    XRT_CALLABLE_FIXED_BYTES, XRT_CALLABLE_NULLABLE, XRT_CALLABLE_SIGNATURE, XRT_CALLABLE_CELL,
    XRT_CALLABLE_TUPLE_I64, XRT_CALLABLE_RECORD
} XrtCallableShapeKind;
typedef struct XrtCallableParameterSpec {
    XrtCallableValueSpec value;
    uint32_t mode;
    bool escaping;
    const char *name;
    uint32_t name_length;
} XrtCallableParameterSpec;
/* Dependencies name earlier receiving rows. Array families are I64, String,
 * U8 and U64. Shape metadata grants no executable address. */
typedef struct XrtCallableTypeSpec {
    XrtCallableShapeKind kind;
    XrtCallableValueSpec element;
    uint32_t count;
    const XrtCallableParameterSpec *parameters;
    uint32_t parameter_count;
    XrtCallableValueSpec result;
    uint32_t flags; /* NoSuspend=1 and C_ABI=2. */
    bool throws;
} XrtCallableTypeSpec;
typedef struct XrtCallableEntrySpec {
    uint32_t type;
    const XrtCallableValueSpec *captures;
    uint32_t capture_count;
} XrtCallableEntrySpec;
/* Compiler-emitted image spans remain immutable and live for the process.
 * Canonical types and entry vectors are independently received by the root. */
typedef struct XrtCallableImage {
    const XrtCallableTypeSpec *types;
    uint32_t type_count;
    const XrtCallableEntrySpec *entries;
    uint32_t entry_count;
} XrtCallableImage;
typedef struct XrtCallableScope {
    const XrtCallableImage *image;
    struct XrtCallableScope *parent;
    void *root, *body;
    uint64_t identity;
} XrtCallableScope;
typedef struct XirFunction XrtCallable;
typedef struct XirCell XrtCapturedCell;
struct XrtMap;
struct XrtModuleState;

#ifdef __cplusplus
extern "C" {
#endif
/* Scopes follow actual synchronous activations. A result takes an independent
 * owner in the parent before the returning scope retires. Failure drains the
 * same rooted scopes without visiting expired automatic C storage. */
XRAY_API void xrt_callable_scope_enter(XrtCallableScope *, const XrtCallableImage *, uint32_t source);
XRAY_API XrtCallable *xrt_callable_scope_new(XrtCallableScope *, uint32_t entry,
    const uint64_t *captures, uint32_t count, uint32_t source);
/* A declaration keeps its independent owner after the producing statement
 * retires. Replacing that binding first receives the new owner, then releases
 * the old one. Binding keys follow declaration order within their scope.
 * A NULL value records a nonowning function and retires any previous owner. */
XRAY_API XrtCallable *xrt_callable_scope_bind(XrtCallableScope *, uint32_t binding,
    XrtCallable *, uint32_t source);
XRAY_API XrtCallable *xrt_callable_scope_leave(XrtCallableScope *, XrtCallable *result, uint32_t source);
XRAY_API uint64_t xrt_callable_capture(const XrtCallableImage *, const XrtCallable *,
    uint32_t entry, uint32_t capture, uint32_t source);
/* A persistent slot owns the canonical object independently. Reading it creates
 * a temporary owner in the actual receiving scope before exposing its handle. */
XRAY_API XrtCallable *xrt_callable_scope_copy(XrtCallableScope *, XrtCallable *, uint32_t source);
XRAY_API uint32_t xrt_callable_entry(const XrtCallableImage *, const XrtCallable *, uint32_t source);
XRAY_API void xrt_callable_state_prepare(struct XrtModuleState *, const XrtCallableImage *, uint32_t source);
/* A captured Cell keeps one canonical identity across borrowed frame captures
 * and owned closures. These pointers borrow the actual scope or closure owner. */
XRAY_API XrtCapturedCell *xrt_callable_cell_scope_new(XrtCallableScope *, uint32_t type, int64_t initial, uint32_t source);
XRAY_API XrtCapturedCell *xrt_callable_cell_scope_map_new(XrtCallableScope *, uint32_t type,
    const struct XrtMap *initial, uint32_t source);
XRAY_API XrtCapturedCell *xrt_callable_cell_scope_bind(XrtCallableScope *, uint32_t binding, XrtCapturedCell *, uint32_t source);
XRAY_API int64_t xrt_callable_cell_i64_read(const XrtCallableImage *, const XrtCapturedCell *, uint32_t source);
XRAY_API void xrt_callable_cell_i64_write(const XrtCallableImage *, const XrtCapturedCell *, int64_t value, uint32_t source);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_CALLABLE_H
