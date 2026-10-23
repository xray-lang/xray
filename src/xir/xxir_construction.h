/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_construction.h - Immutable compiler construction facts
 *
 * KEY CONCEPT:
 *   Dense helper bindings have an explicit immutable owner before complete admission.
 */
/* Immutable compiler construction facts; never runtime or call authority. */
#ifndef XXIR_CONSTRUCTION_H
#define XXIR_CONSTRUCTION_H
#include "xxir.h"
#define XR_XIR_CONSTRUCTION_C_INTERFACE_VERSION 4u

typedef struct XrXirConstruction XrXirConstruction;
/* Complete slot names belong to the actual dense module storage inventory.
 * Names contain exactly name_length UTF-8 bytes, without an embedded NUL.
 * Export observations alone never grant read or execution permission. */
typedef struct XrXirSlotFact {
    uint32_t slot;
    const char *name;
    uint32_t name_length;
    uint32_t exported;
} XrXirSlotFact;
typedef struct XrXirSlotFacts {
    const XrXirSlotFact *rows;
    uint32_t count;
} XrXirSlotFacts;
typedef struct XrXirConstructionRow {
    uint32_t default_initializer;
    const uint32_t *field_initializers;
    uint32_t field_count;
} XrXirConstructionRow;

/* One immutable edge to a private, zero-argument data materializer. Sorted
 * caller/instruction coordinates are interpreted in this exact input graph. */
typedef struct XrXirMaterializationBinding {
    uint32_t caller, instruction, function, nominal;
} XrXirMaterializationBinding;

typedef struct XrXirComptimeSite {
    uint32_t caller,instruction,helper;
} XrXirComptimeSite;
typedef struct XrXirComptimeBinding {
    uint32_t site;
    const XrXirType *arguments;
    uint32_t argument_count,provider;
} XrXirComptimeBinding;
/* Sites are sorted by current caller/instruction. Bindings are sorted by site
 * and ordered local type keys; complete structural checking rejects aliases.
 * Providers are ordinary private literal functions, never cached Values. */
typedef struct XrXirComptimeTable {
    const XrXirComptimeSite *sites;
    uint32_t site_count;
    const XrXirComptimeBinding *bindings;
    uint32_t binding_count;
} XrXirComptimeTable;

typedef struct XrXirConstructionSpec {
    const XrXirConstructionRow *rows;
    uint32_t count;
    const XrXirMaterializationBinding *materializations;
    uint32_t materialization_count;
    const XrXirComptimeTable *comptime;
} XrXirConstructionSpec;

/* Earlier constructor symbols are retired because their input layouts differ.
 * Old compiled clients must fail to link, not misbind.
 * The input has exactly one row per declaration and one value per field.
 * IDs are zero for no binding, otherwise one plus the actual helper function.
 * complete_slots is mandatory, including for zero slots. Each row names its
 * exact dense ordinal. All input rows and name bytes are copied to the receiver.
 * Complete module admission also checks the actual slot denominator and owner.
 * Even a zero-row construction produces a real immutable receiving owner. */
XR_FUNC XrXirStatus xr_xir_compile_construction_new_v4(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirConstructionSpec *input,
    const XrXirSlotFacts *complete_slots, XrXirConstruction **output);
XR_FUNC void xr_xir_compile_construction_free(XrXirConstruction *construction);
XR_FUNC uint32_t xr_xir_compile_construction_count(const XrXirConstruction *construction);
XR_FUNC const XrXirConstructionRow *xr_xir_compile_construction_row(
    const XrXirConstruction *construction, uint32_t ordinal);
/* These observations confer no call, field or construction permission. */
XR_FUNC uint32_t xr_xir_compile_construction_materialization_count(const XrXirConstruction *construction);
XR_FUNC const XrXirMaterializationBinding *xr_xir_compile_construction_materialization(
    const XrXirConstruction *construction, uint32_t ordinal);
XR_FUNC const XrXirConstruction *xr_xir_compile_artifact_construction(const XrXirArtifact *artifact);
XR_FUNC const XrXirComptimeTable *xr_xir_compile_construction_comptime(const XrXirConstruction *construction);
/* Borrowed until this owner is freed. A valid zero-slot owner returns a real
 * view with count zero; a missing owner returns NULL, never fabricated facts. */
XR_FUNC const XrXirSlotFacts *xr_xir_compile_construction_slots(const XrXirConstruction *construction);

#endif // XXIR_CONSTRUCTION_H
