/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_construction_internal.h - Private owned construction representation
 *
 * KEY CONCEPT:
 *   Receiving-ledger clones bind exact nominal kinds and field denominators.
 */
#ifndef XXIR_CONSTRUCTION_INTERNAL_H
#define XXIR_CONSTRUCTION_INTERNAL_H
#include "xxir_construction.h"

struct XrXirConstruction {
    XrXirCompileContext context;
    XrXirConstructionRow *rows;
    uint32_t *kinds;
    uint32_t count;
    XrXirMaterializationBinding *materializations;
    uint32_t materialization_count;
    XrXirComptimeTable comptime;
    XrXirSlotFacts slots;
};
/* An owner is mandatory, including when no nominal declarations exist.
 * A live foreign input owner may be borrowed. Checks and copies charge the
 * receiver; published copies own all facts on that receiver's ledger. */
XR_FUNC XrXirStatus xir_construction_shape(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirConstruction *construction);
/* Both live owners use the same current type/ordinal coordinate space.
 * Complete physical fact correspondence grants no guest equality, module or
 * execution permission. Errors preserve same and never become differences. */
XR_FUNC XrXirStatus xir_construction_compare(const XrXirCompileContext *context,
    const XrXirTypes *types,const XrXirConstruction *left,
    const XrXirConstruction *right,bool *same);
XR_FUNC XrXirStatus xir_construction_clone(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirConstruction *source,
    XrXirConstruction **output);
XR_FUNC XrXirStatus xir_construction_empty(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirConstruction **output);
/* Clear nominal helper bindings while preserving the actual full slot view.
 * The receiving owner still requires complete module admission. */
XR_FUNC XrXirStatus xir_construction_project(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirSlotFacts *slots, XrXirConstruction **output);
XR_FUNC XrXirStatus xir_construction_verify(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirConstruction *construction);
XR_FUNC XrXirStatus xir_materialization_verify(const XrXirCompileContext *context,
    const XrXirModule *module, const XrXirConstruction *construction);
/* Only closed private data recipes need to name substituted opaque types.
 * This classification grants no construction, field or execution authority;
 * the same current verification still checks every instruction and binding.
 * Complete current construction verification must precede this classification. */
XR_FUNC XrXirStatus xir_literal_type_names(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirConstruction *construction,uint32_t function,bool *output);
XR_FUNC const XrXirMaterializationBinding *xir_materialization_binding(
    const XrXirConstruction *construction, uint32_t caller, uint32_t instruction);
XR_FUNC XrXirStatus xir_construction_lower(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirConstruction *source, XrXirConstruction **output);
typedef struct XirComptimeSelection {
    const XrXirModule *source;
    const XrXirTypes *actual_types;
    uint32_t caller,instruction;
    const XrXirType *arguments;
    uint32_t argument_count;
} XirComptimeSelection;
/* Current verified key storage and prepaid vector reads are prerequisites.
 * This order compares metadata only and grants no substitution authority. */
XR_FUNC int xir_comptime_key_order(const XrXirComptimeBinding *a,const XrXirComptimeBinding *b);
XR_FUNC const XrXirComptimeSite *xir_comptime_site(const XrXirConstruction *construction,
    uint32_t caller,uint32_t instruction);
/* Full current module/construction checking precedes selection. No site means
 * no transformation; a site without its actual closed key fails compilation. */
XR_FUNC XrXirStatus xir_comptime_select(const XrXirCompileContext *context,
    const XrXirConstruction *construction,const XirComptimeSelection *input,
    const XrXirComptimeBinding **output);
XR_FUNC XrXirStatus xir_comptime_verify(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirConstruction *construction);
#endif // XXIR_CONSTRUCTION_INTERNAL_H
