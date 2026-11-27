/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_module_interface.h - Source-owned fingerprints for scalar module interfaces
 */
#ifndef XXIR_MODULE_INTERFACE_H
#define XXIR_MODULE_INTERFACE_H

#include "xxir_source.h"
struct XrModuleGraph;

/* Uses the normal Source operation, including optional editor overlays. The
 * canonical name must select a file parsed by that operation. On success the
 * fingerprint describes its scalar interface on the actual target. Failure
 * preserves output. The fingerprint alone grants no reuse or execution. */
XR_FUNC XrXirStatus xir_source_module_interface(const XrXirSourceRequest *,
    const XrModuleOverlayInput *, size_t, const char *canonical, XrFingerprint *output,
    XrXirSourceDiagnostic *);

/* Source calls this before retiring the graph from the same check. */
XR_FUNC XrXirStatus xir_module_interface_project(const struct XrModuleGraph *, const XrXirArtifact *,
    const char *canonical, XrFingerprint *output);

#endif // XXIR_MODULE_INTERFACE_H
