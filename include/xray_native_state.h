/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_state.h - Typed module storage under one execution owner
 *
 * KEY CONCEPT:
 *   Immutable layouts own their complete type context. Execution state owns
 *   every constructed value independently of guest stack and String roots.
 */
#ifndef XRAY_NATIVE_STATE_H
#define XRAY_NATIVE_STATE_H
#include "xray_native_runtime.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XrtModuleLayout XrtModuleLayout;
typedef struct XrtModuleState XrtModuleState;
typedef struct XrtModuleRef XrtModuleRef;
/* Zero denotes success. Nonzero values retain the shared value kernel's exact
 * status and are passed unchanged to xrt_module_require inside a guest root. */
typedef uint32_t XrtModuleStatus;

typedef enum XrtModuleTypeKind {
    XRT_MODULE_TYPE_INVALID,
    XRT_MODULE_UNIT, XRT_MODULE_BOOL, XRT_MODULE_RUNE,
    XRT_MODULE_I8, XRT_MODULE_I16, XRT_MODULE_I32, XRT_MODULE_I64,
    XRT_MODULE_U8, XRT_MODULE_U16, XRT_MODULE_U32, XRT_MODULE_U64,
    XRT_MODULE_F32, XRT_MODULE_F64, XRT_MODULE_STRING,
    XRT_MODULE_ARRAY_I64, XRT_MODULE_ARRAY_STRING, XRT_MODULE_ARRAY_U8, XRT_MODULE_ARRAY_U64,
    XRT_MODULE_FIXED_U8
} XrtModuleTypeKind;
/* Fixed-byte payloads own their exact nonzero count. All other kinds require
 * a zero count. A fixed payload has count bytes, alignment one and no RC. */
typedef struct XrtModuleTypeSpec {
    XrtModuleTypeKind kind;
    uint32_t fixed_count;
} XrtModuleTypeSpec;
typedef struct XrtModuleSpec { uint32_t first_dependency, dependency_count; } XrtModuleSpec;
enum { XRT_MODULE_SLOT_MUTABLE = 1u };
typedef struct XrtModuleSlotSpec {
    uint32_t module, type, flags;
    uint64_t offset;
    uint32_t size, alignment;
} XrtModuleSlotSpec;
typedef struct XrtModuleLayoutSpec {
    const XrtModuleTypeSpec *types;
    uint32_t type_count;
    const XrtModuleSpec *modules;
    uint32_t module_count;
    const uint32_t *dependencies;
    uint32_t dependency_count;
    const XrtModuleSlotSpec *slots;
    uint32_t slot_count;
    uint64_t payload_size, byte_limit;
    uint32_t payload_alignment;
} XrtModuleLayoutSpec;

/* Input arrays are copied. Runtime-selected copy/drop callbacks and their type
 * contexts are owned by the retained layout, with no caller code/context borrow.
 * Counts and indices, canonical nonoverlapping offsets and exact type layouts
 * are validated. Zero byte limits allow zero bytes; UINT64_MAX is unbounded. */
XRAY_API XrtModuleStatus xrt_module_layout_new(const XrtModuleLayoutSpec *, XrtModuleLayout **);
XRAY_API bool xrt_module_layout_retain(XrtModuleLayout *);
XRAY_API void xrt_module_layout_drop(XrtModuleLayout **);
XRAY_API XrtModuleStatus xrt_module_layout_view(const XrtModuleLayout *, XrtModuleLayoutSpec *);
/* The state budget includes its retained layout and all payload/log/scratch
 * storage. A state remains live until its execution owner drops it. */
XRAY_API XrtModuleStatus xrt_module_state_new(XrtModuleLayout *, uint64_t byte_limit, XrtModuleState **);
XRAY_API void xrt_module_state_drop(XrtModuleState **);
XRAY_API XrtModuleStatus xrt_module_begin(XrtModuleState *, uint32_t module);
XRAY_API XrtModuleStatus xrt_module_ready(XrtModuleState *, uint32_t module);
XRAY_API XrtModuleStatus xrt_module_construct(XrtModuleState *, uint32_t slot,
    const void *value, XrtModuleRef **);
XRAY_API XrtModuleStatus xrt_module_publish(XrtModuleRef *);
XRAY_API XrtModuleStatus xrt_module_initialize(XrtModuleState *, uint32_t slot, const void *value);
XRAY_API XrtModuleStatus xrt_module_reference(XrtModuleState *, uint32_t slot, XrtModuleRef **);
XRAY_API XrtModuleStatus xrt_module_borrow(const XrtModuleRef *, const void **value);
/* Copy finishes in empty scratch before the old value is changed. Copy failure
 * releases any partial output, leaves scratch empty and preserves the slot.
 * An initializer reference requires construction; an ordinary one publication.
 * Immutable replacement is rejected. Self-aliasing source values are valid. */
XRAY_API XrtModuleStatus xrt_module_replace(XrtModuleRef *, const void *value);
typedef struct XrtModuleAccess {
    XrtModuleRef *reference;
    void *payload;
    const uint8_t *flags;
    uint8_t required_flags;
    uint32_t size, alignment;
    XrtModuleTypeKind kind;
    bool is_mutable;
} XrtModuleAccess;
/* Cache queries read metadata only. A scalar access checks required_flags at
 * the actual guest instruction before dereferencing payload. String reads use
 * xrt_string_retain to create a guest owner from the independently held value.
 * Managed writes always use the transactional replace operation. */
XRAY_API XrtModuleStatus xrt_module_cache(const XrtModuleState *, uint32_t slot, XrtModuleAccess *);
XRAY_API XrtModuleStatus xrt_module_reference_cache(const XrtModuleRef *, XrtModuleAccess *);
/* Clear pending construction in reverse order, then successful publications
 * in reverse publication order. It allocates nothing and is valid after root
 * String draining, normal return, failure, cancellation or longjmp. */
XRAY_API void xrt_module_state_clear(XrtModuleState *);
XRAY_API void xrt_module_require(XrtModuleStatus, uint32_t source_id);
#ifdef __cplusplus
[[noreturn]] XRAY_API void xrt_module_uninitialized(uint32_t source_id);
#else
_Noreturn XRAY_API void xrt_module_uninitialized(uint32_t source_id);
#endif

typedef void (*XrtSynchronousStateRoot)(void *context, XrtModuleState *state);
/* This entry owns setup and teardown outside its existing recovery jump.
 * Both setup failures and guest failures retire all owners before reporting.
 * The callback synchronously borrows state; retaining a capability is invalid. */
XRAY_API void xrt_run_standalone_state(const XrtSourceMap *, XrtSynchronousStateRoot,
    void *context, const XrtModuleLayoutSpec *);

#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_STATE_H
