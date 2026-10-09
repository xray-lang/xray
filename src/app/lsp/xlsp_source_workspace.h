/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLSP_SOURCE_WORKSPACE_H
#define XLSP_SOURCE_WORKSPACE_H
#include "xlsp_source_navigation.h"
typedef struct XlspSourceWorkspace XlspSourceWorkspace;
typedef struct XlspSourceWorkspaceFailure {
    bool present;
    XrXirSourceDiagnostic diagnostic;
    char *path; /* Owned exact diagnostic module path, if Source supplies it. */
} XlspSourceWorkspaceFailure;
XR_FUNC void xlsp_source_workspace_failure_free(XlspSourceWorkspaceFailure *);
/* One synchronous transaction and one ledger for real stdlib selection,
 * Catalog, authorities, Session, all overlays and root query snapshots.
 * Failure preserves output. No global Session or old analyzer participates. */
XR_FUNC XrXirStatus xlsp_source_workspace_build(const XrXirCompileContext *,
    const XlspSourceDocument *,size_t,XlspSourceWorkspace **,unsigned *stage,XlspSourceWorkspaceFailure *);
XR_FUNC void xlsp_source_workspace_free(XlspSourceWorkspace *);
/* mode 0 definition, 1 references, 2 document highlights, 3 typed hover. JSON owns all text. */
XR_FUNC XrXirStatus xlsp_source_workspace_navigation(XlspSourceWorkspace *,
    const char *,size_t,XrLspPosition,unsigned,bool,XrJsonValue **);
XR_FUNC XrXirStatus xlsp_source_workspace_tokens(XlspSourceWorkspace *,const char *,size_t,
    const XrLspRange *,XlspSourceTokens **);
#endif
