/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_hierarchy_request.h - Owned call and type hierarchy task inputs
 */
#ifndef XLSP_HIERARCHY_REQUEST_H
#define XLSP_HIERARCHY_REQUEST_H
#include "xlsp_source_request.h"
#include "xlsp_server.h"
typedef struct XlspHierarchyRequest XlspHierarchyRequest;
typedef enum XlspSourceScopeQueryKind {
    XLSP_SOURCE_CALL_HIERARCHY,
    XLSP_SOURCE_TYPE_HIERARCHY,
    XLSP_SOURCE_BINDING_LENSES,
    XLSP_SOURCE_SEMANTIC_FULL,
    XLSP_SOURCE_SEMANTIC_DELTA,
    XLSP_SOURCE_SEMANTIC_RANGE,
    XLSP_SOURCE_DOCUMENT_LINKS,
    XLSP_SOURCE_INLAY_BINDINGS,
    XLSP_SOURCE_FOLDING,
    XLSP_SOURCE_SELECTION,
    XLSP_SOURCE_DOCUMENT_SYMBOLS
} XlspSourceScopeQueryKind;
/* Capture borrows editor state only on the dispatch thread. The one-shot run
 * uses canonical Source without accessing the server; hierarchy and CodeLens
 * also scan captured roots. Semantic deltas own a copy of the previous cache.
 * Document links retain the open overlay corpus and canonical import resolver.
 * Params, configuration, open texts and roots survive every producer's death.
 * Cancellation borrows the stable task token only until run returns. */
XR_FUNC XrXirStatus xlsp_hierarchy_request_capture(XrLspServer *, const XrJsonValue *,
    unsigned, XlspSourceScopeQueryKind, XlspHierarchyRequest **);
XR_FUNC XrXirStatus xlsp_hierarchy_request_run(XlspHierarchyRequest *,
    const _Atomic bool *, XlspSourceRequestResult *);
XR_FUNC void xlsp_hierarchy_request_free(XlspHierarchyRequest *);
XR_FUNC XrJsonValue *xlsp_source_scope_query(XrLspServer *, const XrJsonValue *,
    unsigned, XlspSourceScopeQueryKind);
#endif // XLSP_HIERARCHY_REQUEST_H
