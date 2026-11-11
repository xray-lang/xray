/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_types_internal.h - Borrowed descriptor and signature restrictions
 *
 * KEY CONCEPT:
 *   Type shape restricts storage but never proves a live borrow or its origin.
 */
#ifndef XXIR_VIEW_TYPES_INTERNAL_H
#define XXIR_VIEW_TYPES_INTERNAL_H
#include "xxir.h"
#include "xxir_effect_input_internal.h"

#define XR_XIR_VIEW_SHARED 1u
#define XR_XIR_VIEW_MUTABLE 2u
/* The admitted ascending descriptor DAG is shared by compiler and arena caches.
 * The owned packed table uses the caller's finite metadata ledger. */
XR_FUNC XrXirStatus xir_compile_view_flags_new(const XrXirCompileContext *context,
    const XrXirTypes *types, uint8_t **output);
/* Raw pointers add copyable static demand only. This table is not a runtime
 * view layout, affine grant or scope authority and is never cached in an arena. */
XR_FUNC XrXirStatus xir_compile_view_static_flags_new(const XrXirCompileContext *context,
    const XrXirTypes *types,uint8_t **output);

/* Requires an admitted descriptor pool. Tuple and Optional preserve every
 * borrowed leaf; an owned callable's signature is not its capture storage.
 * Output is assigned only after the finite traversal succeeds. */
XR_FUNC XrXirStatus xr_xir_compile_view_type_flags(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirType type, uint32_t *output);
/* Declared callable parameters exclude physical hidden captures. This returns
 * the parameter of the unique signature candidate, not proof that any returned
 * leaf uses it. Each borrowed input leaf counts, including Tuple and Optional.
 * An ordinary result has the canonical UINT32_MAX candidate. */
XR_FUNC XrXirStatus xr_xir_compile_view_return_candidate(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirType callable, uint32_t *output);
/* Requires complete ordinary function structure, type and SSA checks. Replays
 * actual signatures, capture bindings and every returned borrowed leaf.
 * This proves input provenance only, never moves, liveness or access authority. */
XR_FUNC XrXirStatus xir_compile_view_return_origins_verified(const XrXirCompileContext *context,XirEffectInput input, XrXirDiagnostic *diagnostic);
/* Requires ordinary type, structure and SSA admission. Independently checks
 * Shared/Mut owner availability after END, logical affine transfers and PHIs.
 * Shared copies retain independent owners; calls temporarily borrow inputs.
 * This grants neither root/NLL permissions nor executable move semantics. */
XR_FUNC XrXirStatus xir_compile_view_affine_verified(const XrXirCompileContext *context,
    const XrXirModule *module, XrXirDiagnostic *diagnostic);
struct XrXirEffects;
/* Solves availability with point-specific callable loans after private semantic
 * collection. Ordinary type, SSA and origin admission precedes this consumer. */
XR_FUNC XrXirStatus xir_compile_view_affine_loans_verified(const XrXirCompileContext *,
    const XrXirModule *, const struct XrXirEffects *, XrCompileRequestControl *, XrXirDiagnostic *);
/* Requires ordinary structure and type admission. Return origins and boundary
 * types precede private numeric collection. Availability, live intervals and
 * access obligations consume that owner before any checked publication. */
XR_FUNC XrXirStatus xir_compile_view_module_boundary_input(const XrXirCompileContext *context, XirEffectInput input, XrXirDiagnostic *diagnostic);
/* Complete ordinary Module and Construction checks precede the exact replay
 * of every callback reference and use; a signature alone grants no scope. */
XR_FUNC XrXirStatus xir_compile_buffer_scoped_callback(const XrXirCompileContext *context,
    XirEffectInput input,uint32_t function,bool *accepted);
/* Called after declaration and Construction verification. Replays the exact
 * native begin, actual callback reference and nonescaping view use; the normal
 * full function/CFG/root checks remain mandatory before publication. */
XR_FUNC XrXirStatus xir_compile_buffer_scoped_site(const XrXirCompileContext *context,
    XirEffectInput input,uint32_t function,uint32_t instruction,bool *accepted);
#endif // XXIR_VIEW_TYPES_INTERNAL_H
