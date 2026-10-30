/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_specialization_bindings.h - Same-operation declaration correspondence
 */
#ifndef XXIR_SPECIALIZATION_BINDINGS_H
#define XXIR_SPECIALIZATION_BINDINGS_H
#include "xxir.h"

typedef struct XrXirSpecializationMapView {
    const XrXirArtifact *source, *closed;
    const uint32_t *functions;
    uint32_t function_count, slot_count;
    bool identity;
} XrXirSpecializationMapView;
typedef struct XrXirSpecializationReceiver {
    void *context;
    XrXirStatus (*receive)(void *context, const XrXirSpecializationMapView *mapping);
} XrXirSpecializationReceiver;
/* Uses the ordinary specialization and complete receiving verification. The
 * callback runs once before successful publication and before private maps
 * retire. Its borrows end on return. Source function ordinals select actual
 * ordinary destination ordinals; UINT32_MAX has no ordinary physical entry.
 * Only identity=true permits a NULL function table. Slot order is preserved
 * by this producer, with substituted types in the actual closed declarations.
 * Correspondence grants no execution permission. Receiver failure preserves
 * the caller's empty output and releases the unpublished artifact. */
XR_FUNC XrXirStatus xr_xir_compile_specialize_receive(const XrXirArtifact *checked,
    const XrXirSpecializationReceiver *receiver, XrXirArtifact **output, XrXirDiagnostic *diagnostic, XrCompileRequestControl *control);
#endif // XXIR_SPECIALIZATION_BINDINGS_H
