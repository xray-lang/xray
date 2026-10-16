/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "before_query.c"
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
