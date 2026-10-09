/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_construction_fixture.h - Mandatory owned facts for raw test modules
 *
 * KEY CONCEPT:
 *   Raw fixtures validate their types before creating dense zero facts.
 *   Nonzero construction permissions require the actual immutable owner.
 */
#ifndef XIR_CONSTRUCTION_FIXTURE_H
#define XIR_CONSTRUCTION_FIXTURE_H
#include "../../../src/xir/xxir_internal.h"
#include "../../../src/xir/xxir_types.h"
#include "../../../src/xir/xxir_compile_memory.h"
#include "../../../src/xir/xxir_source_query_internal.h"
static inline XrXirStatus xir_fixture_construction(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirConstruction **output) {
    /* Admit raw fixture storage before deriving the dense zero denominator. */
    XrXirStatus status = xr_xir_compile_types_structure_verify(context, types);
    return status == XR_XIR_OK ? xir_construction_empty(context, types, output) : status;
}
static inline bool xir_fixture_invalid_prefix(const XrXirCompileContext *context,
    const XrXirModule *module) {
    return !xir_compile_context_valid(context) || !module || !module->functions || !module->function_count ||
        (module->linkage_kind!=XR_XIR_PROGRAM && module->linkage_kind!=XR_XIR_LIBRARY) ||
        (module->stage!=XR_XIR_BUILT && module->stage!=XR_XIR_CHECKED && module->stage!=XR_XIR_LOWERED) ||
        module->function_count>context->limits.functions;
}
static inline void xir_fixture_preparation_diagnostic(XrXirStatus status,XrXirDiagnostic *diagnostic) {
    if(diagnostic)*diagnostic=(XrXirDiagnostic){status,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
}
static inline XrXirStatus xir_fixture_check(const XrXirCompileContext *context,
    const XrXirModule *module, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (xir_fixture_invalid_prefix(context,module) || !output || *output || module->stage!=XR_XIR_BUILT)
        return xr_xir_compile_check_v2(context,module,NULL,output,diagnostic);
    XrXirConstruction *facts = NULL;
    XrXirStatus status = xir_fixture_construction(context, module->types, &facts);
    if (status == XR_XIR_OK) status = xr_xir_compile_check_v2(context, module, facts, output, diagnostic);
    else xir_fixture_preparation_diagnostic(status,diagnostic);
    xr_xir_compile_construction_free(facts);
    if (diagnostic) diagnostic->status = status;
    return status;
}
static inline XrXirStatus xir_fixture_verify(const XrXirCompileContext *context,
    const XrXirModule *module, XrXirDiagnostic *diagnostic) {
    if (xir_fixture_invalid_prefix(context,module))
        return xr_xir_compile_verify_v2(context,module,NULL,diagnostic);
    XrXirConstruction *facts = NULL;
    XrXirStatus status = xir_fixture_construction(context, module->types, &facts);
    if (status == XR_XIR_OK) status = xr_xir_compile_verify_v2(context, module, facts, diagnostic);
    else xir_fixture_preparation_diagnostic(status,diagnostic);
    xr_xir_compile_construction_free(facts);
    if (diagnostic) diagnostic->status = status;
    return status;
}
static inline XrXirStatus xir_fixture_recheck(const XrXirCompileContext *context,
    const XrXirModule *module, XrXirArtifact **output, XrXirDiagnostic *diagnostic) {
    if (xir_fixture_invalid_prefix(context,module) || !output || *output || module->stage!=XR_XIR_CHECKED)
        return xr_xir_compile_recheck_v2(context,module,NULL,output,diagnostic);
    XrXirConstruction *facts = NULL;
    XrXirStatus status = xir_fixture_construction(context, module->types, &facts);
    if (status == XR_XIR_OK) status = xr_xir_compile_recheck_v2(context, module, facts, output, diagnostic);
    else xir_fixture_preparation_diagnostic(status,diagnostic);
    xr_xir_compile_construction_free(facts);
    if (diagnostic) diagnostic->status = status;
    return status;
}
/* Synthetic observation records are not an executable Artifact. The copy
 * fixture supplies real zero facts and retains all original raw-data checks. */
static inline XrXirStatus xir_fixture_snapshot_copy(const XrXirCompileContext *context,
    const XrXirSourceView *view, XrXirSourceSnapshot **output) {
    if (!view || !output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirConstruction *facts = NULL;
    /* Raw observation fixtures validate descriptor storage before dense reads. */
    XrXirStatus status = xir_fixture_construction(context, view->types, &facts);
    if (status == XR_XIR_OK) status = xr_xir_compile_source_snapshot_copy_v2(context, view, facts, output);
    xr_xir_compile_construction_free(facts);
    return status;
}
#endif // XIR_CONSTRUCTION_FIXTURE_H
