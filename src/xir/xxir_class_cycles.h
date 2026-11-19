/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_class_cycles.h - Allocation classification after specialization
 */
#ifndef XXIR_CLASS_CYCLES_H
#define XXIR_CLASS_CYCLES_H
#include "xxir.h"

typedef enum XrXirClassCycleKind {
    XR_XIR_CLASS_CYCLE_UNKNOWN,
    XR_XIR_CLASS_CYCLE_POSSIBLE,
    XR_XIR_CLASS_CYCLE_ACYCLIC
} XrXirClassCycleKind;

typedef struct XrXirClassCycles XrXirClassCycles;

/* Accepts a live specialized Checked program. The table borrows that exact
 * artifact through disposal. Only the actual instantiated class type in that
 * artifact can be queried. UNKNOWN and POSSIBLE both require a cycle domain;
 * this proof alone supplies no construction or publication authority. */
XR_FUNC XrXirStatus xr_xir_class_cycles_build(const XrXirArtifact *, XrXirClassCycles **empty,
    XrCompileRequestControl *);
XR_FUNC XrXirStatus xr_xir_class_cycles_query(const XrXirClassCycles *, const XrXirArtifact *,
    XrXirType, XrXirClassCycleKind *);
XR_FUNC void xr_xir_class_cycles_free(XrXirClassCycles *);
#endif // XXIR_CLASS_CYCLES_H
