/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLSP_SOURCE_OPEN_H
#define XLSP_SOURCE_OPEN_H
#include "xlsp_source_buffer.h"
#include "xlsp_source_syntax.h"
#include "xlsp_source_workspace.h"
typedef struct XlspSourceOpen XlspSourceOpen;
/* All current documents plus the new/replaced entry. Operational failures do
 * not publish; real language diagnostics publish an editable owned error state,
 * never a successful semantic query. One context ledger covers every phase. */
XR_FUNC XrXirStatus xlsp_source_open_prepare(const XrXirCompileContext *,const XlspSourceDocument *,size_t,size_t,XlspSourceOpen **);
XR_FUNC void xlsp_source_open_free(XlspSourceOpen *);
XR_FUNC const char *xlsp_source_open_uri(const XlspSourceOpen *);
XR_FUNC XrXirStatus xlsp_source_open_query_status(const XlspSourceOpen *);
XR_FUNC const XlspSourceWorkspaceFailure *xlsp_source_open_failure(const XlspSourceOpen *);
/* Exactly-once ownership transfer after the transaction is ready to commit. */
XR_FUNC XlspSourceBuffer *xlsp_source_open_take_buffer(XlspSourceOpen *);
XR_FUNC XlspSyntaxSnapshot *xlsp_source_open_take_syntax(XlspSourceOpen *);
XR_FUNC XlspSourceWorkspace *xlsp_source_open_take_workspace(XlspSourceOpen *);
XR_FUNC XrXirStatus xlsp_source_open_diagnostics_json(const XlspSourceOpen *,const XlspSourceBuffer *,const XlspSyntaxSnapshot *,XrJsonValue **);
#endif
