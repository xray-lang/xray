/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_access_internal.h - Independent borrowed access conflict decisions
 *
 * KEY CONCEPT:
 *   Unknown epochs or aliases remain unresolved rather than language errors.
 */
#ifndef XXIR_VIEW_ACCESS_INTERNAL_H
#define XXIR_VIEW_ACCESS_INTERNAL_H
#include "xxir.h"
#include "xxir_effect_input_internal.h"
#include "xxir_cell_provenance_internal.h"
#include "xxir_effect_contract_internal.h"
/* Borrowed only during the composite verifier call. The opaque owner and
 * reader come from the common Cell producer for this exact immutable module,
 * never from caller-provided role bits. No fact escapes as entry authority. */
typedef struct XirViewAccessCells {
    const XrXirCellProvenance *owner;
    XrXirCellProofRole (*role)(const XrXirCellProvenance *,uint32_t,uint32_t);
    /* This synchronous borrow reads the same immutable numeric semantic owner.
     * Missing facts leave hidden access unresolved. */
    const struct XrXirEffects *effects;
    /* Synchronous request observation borrows the current input only. It is
     * never retained by the resolver or any published proof owner. */
    XrCompileRequestControl *control;
    const XrXirConstruction *construction;
} XirViewAccessCells;
/* A synchronous numeric query does not retain the effect owner or grant a
 * borrowed access. The access consumer still checks the actual live loans. */
XR_FUNC XrXirStatus xir_effects_value_restricted(const XrXirCompileContext *context,
    const struct XrXirEffects *effects, uint32_t function, uint32_t value, bool *output);
/* The binding identity belongs to the same qualified origin equations that
 * callers use; this query grants neither lifetime nor disjointness. */
XR_FUNC XrXirStatus xir_effects_module_origin(const XrXirCompileContext *,
    const struct XrXirEffects *, uint32_t, uint32_t, uint32_t, bool *);
/* A conditional callee entry reads its actual module-write summary. The common
 * receiver still checks every caller's borrowed owner against that summary. */
XR_FUNC XrXirStatus xir_effects_view_module_entry(const XrXirCompileContext *,
    const struct XrXirEffects *, const XrXirModule *, uint32_t, uint32_t, uint32_t, bool *);
/* Requires ordinary type/structure/SSA, return-origin and affine admission of
 * the immutable generation. Recomputes actual may-owned state, scope ancestry,
 * compatible PHI choices and storage paths. Borrowed inputs denote compatible
 * temporary grants installed by their common actual Call entry; checking a
 * body under those grants does not independently admit an entry or caller.
 * BAD_VALUE reports a proven access
 * conflict; UNRESOLVED preserves missing epoch, alias or boundary obligations.
 * OK covers these access checks only, not root lifetime, effects, foreign or
 * suspension authority, executable admission or any other consumer proof. */
XR_FUNC XrXirStatus xir_compile_view_access_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirDiagnostic *diagnostic);
/* Internal receiver of the same-generation owner described above. The joined
 * entry below alone creates that owner and charges its simultaneous residency. */
XR_FUNC XrXirStatus xir_compile_view_access_cells_verified(const XrXirCompileContext *context,
    const XrXirModule *module,const XirViewAccessCells *cells,XrXirDiagnostic *diagnostic);
XR_FUNC XrXirStatus xir_compile_view_cell_access_verified_input(const XrXirCompileContext *context, XirEffectInput input,const struct XrXirEffects *effects,XrXirDiagnostic *diagnostic);
#endif // XXIR_VIEW_ACCESS_INTERNAL_H
