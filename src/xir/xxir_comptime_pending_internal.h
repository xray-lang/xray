/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_comptime_pending_internal.h - Owned unresolved specialization observations
 *
 * KEY CONCEPT:
 *   Ordinary specialization identifies an actual missing canonical key. The
 *   observation carries no executable graph, Value or validation authority.
 */
#ifndef XXIR_COMPTIME_PENDING_INTERNAL_H
#define XXIR_COMPTIME_PENDING_INTERNAL_H
#include "xxir_construction.h"
typedef struct XirComptimePending XirComptimePending;
typedef struct XirComptimePendingView {
    const XrXirArtifact *definition;
    const XrXirTypes *types;
    XrXirComptimeSite site;
    const XrXirType *arguments;
    uint32_t argument_count;
} XirComptimePendingView;
/* A NULL owner on success means every actually visited key was resolved.
 * The input is genuine Checked Program Source. The original finite compiler
 * ledger owns independent Source bytes, the full pool and ordered arguments.
 * Existing bindings retain their meaning. No table or TEMPLATE is removed.
 * An unresolved observation is neither Checked instance nor Lowered input. */
XR_FUNC XrXirStatus xr_xir_compile_comptime_pending(const XrXirArtifact *definition,
    XirComptimePending **output,XrXirDiagnostic *diagnostic, XrCompileRequestControl *control);
/* Current complete Source bytes, descriptor validity and ordinary discovery
 * are checked again before an atomic borrow. Keep owner live and immutable;
 * no cached PASS, caller type vector, AST or supplied Value enters. */
XR_FUNC XrXirStatus xr_xir_compile_comptime_pending_view(const XirComptimePending *owner,
    XirComptimePendingView *output,XrXirDiagnostic *diagnostic, XrCompileRequestControl *control);
XR_FUNC void xr_xir_compile_comptime_pending_free(XirComptimePending *owner);
/* Full ordinary closure and independent correspondence produce a compiler-only
 * evaluation artifact. Its fixed missing helper is the sole evaluation root;
 * ordinary transport and runtime consumers cannot accept this purpose. */
XR_FUNC XrXirStatus xr_xir_compile_comptime_pending_specialize(const XirComptimePending *owner,
    XrXirArtifact **output,XrXirDiagnostic *diagnostic, XrCompileRequestControl *control);
#endif // XXIR_COMPTIME_PENDING_INTERNAL_H
