/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_internal.h - Private ownership of immutable XIR artifacts
 *
 * KEY CONCEPT:
 *   Only checked stage transitions construct physical layout tables.
 */

#ifndef XXIR_INTERNAL_H
#define XXIR_INTERNAL_H

#include "xxir.h"

struct XrXirArtifact {
    XrXirModule module;
    XrXirBudget budget;
    XrXirTarget target;
    XrXirFunctionLayout *layouts;
};

XR_FUNC XrXirStatus xr_xir_layout_build(XrXirArtifact *artifact, const XrXirBudget *budget);
XR_FUNC XrXirStatus xr_xir_layout_verify(const XrXirArtifact *artifact, const XrXirBudget *budget);

#endif // XXIR_INTERNAL_H
