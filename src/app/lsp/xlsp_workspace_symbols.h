/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLSP_WORKSPACE_SYMBOLS_H
#define XLSP_WORKSPACE_SYMBOLS_H
#include "xlsp_source_request.h"
#include "xlsp_server.h"
typedef struct XlspWorkspaceSymbolRequest XlspWorkspaceSymbolRequest;
/* The dispatch thread copies display facts and open-version admission only.
 * The one-shot worker never borrows the index store, documents or server.
 * Its query ledger covers capture, matching and complete JSON publication. */
XR_FUNC XrXirStatus xlsp_workspace_symbols_capture(XrLspServer *,const XrJsonValue *,XlspWorkspaceSymbolRequest **);
XR_FUNC XrXirStatus xlsp_workspace_symbols_run(XlspWorkspaceSymbolRequest *,const _Atomic bool *,XlspSourceRequestResult *);
XR_FUNC void xlsp_workspace_symbols_free(XlspWorkspaceSymbolRequest *);
XR_FUNC XrJsonValue *xlsp_workspace_symbols_handle(XrLspServer *,const XrJsonValue *);
#endif // XLSP_WORKSPACE_SYMBOLS_H
