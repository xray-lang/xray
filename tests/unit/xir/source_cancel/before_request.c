/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_source_request.h"
/* Use the real request implementation with only interior control omitted. */
static XrXirStatus before_workspace_build(const XrXirCompileContext *context,
    const XlspSourceDocument *documents,size_t count,XlspSourceWorkspace **output,
    unsigned *stage,XlspSourceWorkspaceFailure *failure,XrCompileRequestControl *control) {
    (void)control;
    return xlsp_source_workspace_build(context,documents,count,output,stage,failure,NULL);
}
#define xlsp_source_workspace_build before_workspace_build
#include "../../../../src/app/lsp/xlsp_source_request.c"
#undef xlsp_source_workspace_build
