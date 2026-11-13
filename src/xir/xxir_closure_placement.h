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
    /* Observed rows retain the execution call and its qualified caller. An
     * unobserved template seed uses MAX call/context and the template instance. */
    uint32_t constructor_call, caller_instance, entry_context;
} XirClosurePlacement;

static inline int xir_closure_placement_compare(const XirClosurePlacement *a, const XirClosurePlacement *b) {
    if (a->function != b->function) return a->function < b->function ? -1 : 1;
    if (a->value != b->value) return a->value < b->value ? -1 : 1;
    if (a->environment != b->environment) return a->environment < b->environment ? -1 : 1;
    if (a->constructor_call != b->constructor_call) return a->constructor_call < b->constructor_call ? -1 : 1;
    if (a->caller_instance != b->caller_instance) return a->caller_instance < b->caller_instance ? -1 : 1;
    if (a->entry_context != b->entry_context) return a->entry_context < b->entry_context ? -1 : 1;
    return 0;
}

/* Borrows the immutable Artifact owner. Queries never allocate, infer, link or
 * revisit a body. Failure preserves outputs. The scalar query joins all actual
 * contexts of one physical constructor for a shared code-generation body.
 * A specialized constructor uses its verified value correspondence and the
 * retained source owner. Its application ordinals are never reinterpreted. */
XR_FUNC XrXirStatus xr_xir_compile_closure_placements(const XrXirArtifact *,
    const XirClosurePlacement **output, uint32_t *count);
XR_FUNC XrXirStatus xr_xir_compile_closure_placement(const XrXirArtifact *,
    uint32_t function, uint32_t value, XirClosureStorage *output);
#endif // XXIR_CLOSURE_PLACEMENT_H
