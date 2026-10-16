/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_folding.h"
#include "app/lsp/xlsp_document_store.h"
#include "api/xsource_compile.h"
XR_FUNC XrCompileResourceStatus task_resource_new_impl(const XrCompileResourceLimits *,XrCompileResources **);
/* The predecessor uses its own default limits; preserve them in the control. */
#define xr_compile_resources_new task_resource_new_impl
#define xlsp_handle_folding_range before_xlsp_handle_folding_range
#include "before_folding.c"
#undef xlsp_handle_folding_range
#undef xr_compile_resources_new
/* Match the document owner's lookup without importing its unrelated consumers. */
XR_FUNC XrLspDocument *xlsp_document_get(XrLspServer *server,const char *uri) {
    return server?xlsp_document_store_get(server->doc_table,uri):NULL;
}
