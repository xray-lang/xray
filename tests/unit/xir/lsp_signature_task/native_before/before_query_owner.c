/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_source_query.h"
/* The historical query has no display sidecar. Keep its bytes and bridge only
 * the changed owning failure argument; this control cannot publish new facts. */
static XrXirStatus native_before_overlay(const XrXirSourceRequest *request,
    const XrModuleOverlayInput *inputs,size_t count,XrXirSourceResult *out,
    XrXirSourceDiagnostic *diagnostic,char **path) {
    if(path&&*path)return XR_XIR_BAD_STRUCTURE;
    XrXirSourceFailure failure={0};
    XrXirStatus status=xr_xir_compile_source_check_overlay(request,inputs,count,out,diagnostic,path?&failure:NULL);
    if(path){*path=failure.path;failure.path=NULL;}
    xr_xir_compile_source_failure_free(&failure);return status;
}
#define xr_xir_compile_source_check_overlay native_before_overlay
#define xlsp_source_snapshot_build_authorities native_before_build_authorities
#define xlsp_source_snapshot_build native_before_build
#include "before_query.c"
#undef xlsp_source_snapshot_build
#undef xlsp_source_snapshot_build_authorities
#undef xr_xir_compile_source_check_overlay
/* Preserve the existing builder interfaces while the retired query owns only
 * its original path result. The adapter does not add parser-display authority. */
XR_FUNC XrXirStatus xlsp_source_snapshot_build_authorities(const XrXirSourceRequest *request,
    const XlspSourceDocument *documents,const XrModuleIdentityAuthority *authorities,size_t count,size_t entry,
    XlspSourceSnapshot **out,XrXirSourceDiagnostic *diagnostic,XrXirSourceFailure *failure) {
    if(!xr_xir_compile_source_failure_empty(failure))return XR_XIR_BAD_STRUCTURE;
    char *path=NULL;XrXirStatus status=native_before_build_authorities(request,documents,authorities,count,entry,out,diagnostic,failure?&path:NULL);
    if(failure)failure->path=path;return status;
}
XR_FUNC XrXirStatus xlsp_source_snapshot_build(const XrXirSourceRequest *request,
    const XlspSourceDocument *documents,size_t count,size_t entry,XlspSourceSnapshot **out,
    XrXirSourceDiagnostic *diagnostic,XrXirSourceFailure *failure) {
    if(!xr_xir_compile_source_failure_empty(failure))return XR_XIR_BAD_STRUCTURE;
    char *path=NULL;XrXirStatus status=native_before_build(request,documents,count,entry,out,diagnostic,failure?&path:NULL);
    if(failure)failure->path=path;return status;
}
#include "xlsp_source_inlay.h"
/* Preserve the predecessor query bytes and expose its existing ledger identity. */
XR_FUNC bool xlsp_source_snapshot_uses_resources(const XlspSourceSnapshot *snapshot,const XrCompileResources *resources) {
    return snapshot&&resources&&snapshot->resources==resources;
}
/* The predecessor has no parameter-hint producer; reject that new capability explicitly. */
XR_FUNC XrXirStatus xlsp_source_parameter_hints(const XlspSourceSnapshot *snapshot,
    const XlspSourceCallArguments *arguments,XrLspRange range,XlspSourceParameterHints *out) {
    (void)snapshot;(void)arguments;(void)range;(void)out;return XR_XIR_UNSUPPORTED;
}
XR_FUNC void xlsp_source_parameter_hints_free(XlspSourceParameterHints *hints) {
    (void)hints;
}

XR_FUNC XrXirStatus xlsp_source_type_hints(const XlspSourceSnapshot *snapshot,const char *uri,size_t length,
    XrLspRange range,XlspSourceTypeHints *out) {
    (void)snapshot;(void)uri;(void)length;(void)range;(void)out;return XR_XIR_UNSUPPORTED;
}
XR_FUNC void xlsp_source_type_hints_free(XlspSourceTypeHints *hints) {(void)hints;}
