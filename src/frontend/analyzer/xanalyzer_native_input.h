/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_native_input.h - Explicit same-ledger native semantic input
 */
#ifndef XANALYZER_NATIVE_INPUT_H
#define XANALYZER_NATIVE_INPUT_H
#include "xanalyzer.h"
#include "../../module/xnative_package.h"
#include "../../toolchain/xcompiler_session.h"

/* Plan must come from the owned manifest constructor on the same ledger.
 * The caller keeps it live and excludes mutation during semantic queries.
 * Layout preparation may mutate its existing layout facts through the borrow.
 * No plan is an explicit absence, never an invented complete contract.
 * Rejected binding preserves prior input; an existing resource failure wins. */
XR_FUNC XrCompileResourceStatus xa_analyzer_bind_native_plan(XaAnalyzer *analyzer,
    XrNativePackagePlan *plan);
/* Borrow ends with plan mutation/destruction. Failure preserves output. */
XR_FUNC XrManifestStatus xa_analyzer_find_native_symbol(XaAnalyzer *analyzer,
    const char *name, const XrNativeSymbol **output);
/* Semantic arity rejection returns INVALID. Its optional diagnostic is only
 * replaced after successful rendering; a known rejection remains the first
 * cause if that rendering exhausts work. Earlier resource failure poisons the
 * original CompileState and preserves the caller's diagnostic. */
XR_FUNC XrManifestStatus xa_analyzer_validate_native_arity(XaAnalyzer *analyzer,
    const char *name, uint32_t arity, XrManifestDiagnostic *diagnostic);
/* Layout updates stage their owned strings before replacing the plan's facts.
 * Failure preserves the previous plan and, for resolution, the matched flag. */
XR_FUNC XrManifestStatus xa_analyzer_note_native_layout_subject(XaAnalyzer *analyzer,
    const char *name);
XR_FUNC XrManifestStatus xa_analyzer_resolve_native_layout(XaAnalyzer *analyzer,
    const char *name, const struct XrAggregateLayout *layout, bool *matched);
static inline XrNativePackagePlan *xa_analyzer_native_plan(const XaAnalyzer *analyzer) {
    return analyzer && analyzer->native_plan_bound &&
        xr_compile_session_resource_status(analyzer->compiler_session) == XR_COMPILE_RESOURCE_OK
        ? analyzer->native_plan : NULL;
}
#endif // XANALYZER_NATIVE_INPUT_H
