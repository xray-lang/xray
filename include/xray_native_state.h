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
#include "xray_native_place.h"
#include "xray_native_value_image.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XrtModuleLayout XrtModuleLayout;
typedef struct XrtModuleState XrtModuleState;
typedef struct XrtModuleRef XrtModuleRef;
typedef XrtManagedStatus XrtModuleStatus;
typedef struct XrtModuleSpec { uint32_t first_dependency, dependency_count; } XrtModuleSpec;
enum { XRT_MODULE_SLOT_MUTABLE = 1u };
typedef struct XrtModuleSlotSpec {
    uint32_t module, type, flags;
    size_t offset, size, alignment;
} XrtModuleSlotSpec;
typedef struct XrtModuleLayoutSpec {
    const XrtValueImage *values;
    const XrtModuleSpec *modules;
    uint32_t module_count;
    const uint32_t *dependencies;
    uint32_t dependency_count;
    const XrtModuleSlotSpec *slots;
    uint32_t slot_count;
    size_t payload_size, payload_alignment;
} XrtModuleLayoutSpec;
/* A bound view borrows owned metadata, never the raw receiving image. Type ids
 * remain one-based ordinals in that exact image. */
typedef struct XrtModuleLayoutView {
    uint32_t type_count;
    const XrtModuleSpec *modules;
    uint32_t module_count;
    const uint32_t *dependencies;
    uint32_t dependency_count;
    const XrtModuleSlotSpec *slots;
    uint32_t slot_count;
    size_t payload_size, payload_alignment;
} XrtModuleLayoutView;
/* Binding copies all input descriptions and names through the supplied billing
 * owner. Code pins the raw input throughout provider callbacks; only plans that
 * need executable code keep that lease afterwards. Layout never owns State.
 * All payload owner fields are declared XrtManagedHeader*. Function payloads
 * are complete caller-admitted values with factory-owned environments; the
 * caller also admits their exact signature and executable entry lifetime.
 * Environment and Cell rows are metadata context, not direct State payloads.
 * View payloads still require their complete target plans. */
XRAY_API XrtModuleStatus xrt_module_layout_new(XrtBillingOwner *, const XrtModuleLayoutSpec *,
    XrtManagedLease code, XrtModuleLayout **);
XRAY_API bool xrt_module_layout_retain(XrtModuleLayout *);
XRAY_API void xrt_module_layout_drop(XrtModuleLayout **);
XRAY_API XrtModuleStatus xrt_module_layout_view(const XrtModuleLayout *, XrtModuleLayoutView *);
/* State allocation and scratch are charged to this actual execution owner,
 * independently of the retained immutable layout's billing owner. */
XRAY_API XrtModuleStatus xrt_module_state_new(XrtModuleLayout *, XrtBillingOwner *, XrtModuleState **);
XRAY_API void xrt_module_state_drop(XrtModuleState **);
XRAY_API XrtModuleStatus xrt_module_begin(XrtModuleState *, uint32_t module);
XRAY_API XrtModuleStatus xrt_module_ready(XrtModuleState *, uint32_t module);
XRAY_API XrtModuleStatus xrt_module_construct(XrtModuleState *, uint32_t slot,
    const void *value, XrtModuleRef **);
XRAY_API XrtModuleStatus xrt_module_publish(XrtModuleRef *);
XRAY_API XrtModuleStatus xrt_module_initialize(XrtModuleState *, uint32_t slot, const void *value);
/* Validate the complete contiguous I64 group before publishing any slot.
 * Mutable members must already be constructed by this initializer. The call
 * preserves their current payloads, including writes before publication.
 * Immutable members receive contiguous I64 object bytes through budgeted
 * scratch before the whole publication commits. The span must cover exactly
 * count values. Failure preserves prior construction. */
XRAY_API XrtModuleStatus xrt_module_initialize_i64_group(XrtModuleState *, uint32_t first,
    uint32_t count, const void *values, size_t value_size);
XRAY_API XrtModuleStatus xrt_module_reference(XrtModuleState *, uint32_t slot, XrtModuleRef **);
XRAY_API XrtModuleStatus xrt_module_borrow(const XrtModuleRef *, const void **value);
/* The destination is stable registered owning storage, or an owner synchronously
 * retired before any nonlocal failure. Its leaves start NULL. Failure preserves
 * both payloads; success makes an independent snapshot without allocating. */
XRAY_API XrtModuleStatus xrt_module_copy(const XrtModuleRef *, void *empty_value);
/* A binding place does not grant mutation authority. Existing admission proves
 * the borrow; its guard only checks readiness/mutability and forbids MOVE_FROM. */
XRAY_API XrtModuleStatus xrt_module_external_owner(const XrtModuleRef *, size_t leaf,
    XrtExternalOwner *);
/* Inputs are admitted typed values and remain borrowed through this call; they
 * may alias the binding. Copy finishes in empty scratch before the old value is
 * changed. Failure leaves scratch empty and preserves the complete binding.
 * An initializer reference requires construction; an ordinary one publication.
 * Immutable replacement is rejected. Self-aliasing source values are valid. */
XRAY_API XrtModuleStatus xrt_module_replace(XrtModuleRef *, const void *value);
typedef struct XrtModuleAccess {
    XrtModuleRef *reference;
    void *payload;
    const uint8_t *flags;
    uint8_t required_flags;
    size_t size, alignment;
    uint32_t type;
    XrtValueKind kind;
    bool is_mutable;
} XrtModuleAccess;
/* Cache queries read metadata only. A scalar access checks required_flags at
 * the actual guest instruction before dereferencing payload. Ordinary owned
 * reads use xrt_module_copy to obtain an independent snapshot.
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
