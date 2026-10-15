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
#define XR_XIR_CONSTRUCTION_C_INTERFACE_VERSION 2u

typedef struct XrXirConstruction XrXirConstruction;
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

typedef struct XrXirConstructionSpec {
    const XrXirConstructionRow *rows;
    uint32_t count;
    const XrXirMaterializationBinding *materializations;
    uint32_t materialization_count;
} XrXirConstructionSpec;

/* The unversioned constructor symbol is retired because its input layout and
 * argument count differ. Old compiled clients must fail to link, not misbind.
 * The input has exactly one row per declaration and one value per field.
 * IDs are zero for no binding, otherwise one plus the actual helper function.
 * Even a zero-row construction produces a real immutable receiving owner. */
XR_FUNC XrXirStatus xr_xir_compile_construction_new_v2(const XrXirCompileContext *context,
    const XrXirTypes *types, const XrXirConstructionSpec *input,
    XrXirConstruction **output);
XR_FUNC void xr_xir_compile_construction_free(XrXirConstruction *construction);
XR_FUNC uint32_t xr_xir_compile_construction_count(const XrXirConstruction *construction);
XR_FUNC const XrXirConstructionRow *xr_xir_compile_construction_row(
    const XrXirConstruction *construction, uint32_t ordinal);
/* These observations confer no call, field or construction permission. */
XR_FUNC uint32_t xr_xir_compile_construction_materialization_count(const XrXirConstruction *construction);
XR_FUNC const XrXirMaterializationBinding *xr_xir_compile_construction_materialization(
    const XrXirConstruction *construction, uint32_t ordinal);
XR_FUNC const XrXirConstruction *xr_xir_compile_artifact_construction(const XrXirArtifact *artifact);

#endif // XXIR_CONSTRUCTION_H
