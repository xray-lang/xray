/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XXIR_CLOSURE_PLACEMENT_H
#define XXIR_CLOSURE_PLACEMENT_H
#include "xxir.h"

typedef enum XirClosureStorage {
    XIR_CLOSURE_FRAME, XIR_CLOSURE_OWNED, XIR_CLOSURE_UNRESOLVED
} XirClosureStorage;
/* Numeric placement is independent of R/S permissions. Context and occurrence
 * age remain qualified even when a receiving backend needs their upper bound. */
typedef struct XirClosurePlacement {
    uint32_t function, value, environment, ages, remainder;
    XirClosureStorage storage;
} XirClosurePlacement;

/* Borrows the immutable Artifact owner. Queries never allocate, infer, link or
 * revisit a body. Failure preserves outputs. The scalar query joins all actual
 * contexts of one physical constructor for a shared code-generation body. */
XR_FUNC XrXirStatus xr_xir_compile_closure_placements(const XrXirArtifact *,
    const XirClosurePlacement **output, uint32_t *count);
XR_FUNC XrXirStatus xr_xir_compile_closure_placement(const XrXirArtifact *,
    uint32_t function, uint32_t value, XirClosureStorage *output);
#endif // XXIR_CLOSURE_PLACEMENT_H
