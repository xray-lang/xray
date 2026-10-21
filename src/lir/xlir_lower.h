/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower.h - Owned scalar lowering from source-backed Closed Checked XIR
 *
 * KEY CONCEPT:
 *   Checked inputs are borrowed synchronously. Both successful modules and
 *   failed diagnostics own the metadata they publish to their caller.
 */
#ifndef XLIR_LOWER_H
#define XLIR_LOWER_H
#include "xlir.h"

typedef struct XrXirArtifact XrXirArtifact;
typedef struct XrXirSourceSnapshot XrXirSourceSnapshot;
typedef struct XrLirLowerRequest {
    const XrXirArtifact *closed;
    const XrXirSourceSnapshot *snapshot;
    bool overflow_checks;
} XrLirLowerRequest;
typedef struct XrLirLowered {
    XrLirModule *module;
    XrLirFunction entry;
} XrLirLowered;
typedef struct XrLirLowerFailure {
    XrLirDiagnostic diagnostic;
    char *owned_path;
} XrLirLowerFailure;

/* Output and optional failure must initially be empty. The snapshot must
 * correspond to the source of the supplied specialization. An unsupported
 * operation never publishes a partial module or selects another executor. */
XR_FUNC XrLirStatus xr_lir_lower_checked(const XrLirLowerRequest *, XrLirLowered *,
                                         XrLirLowerFailure *);
XR_FUNC void xr_lir_lowered_free(XrLirLowered *);
XR_FUNC void xr_lir_lower_failure_free(XrLirLowerFailure *);

#endif  // XLIR_LOWER_H
