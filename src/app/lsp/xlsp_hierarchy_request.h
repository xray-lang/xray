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
/* Capture borrows editor state only on the dispatch thread. The one-shot run
 * scans captured roots and uses canonical Source without accessing the server.
 * Params, configuration, open texts and roots survive every producer's death.
 * Cancellation borrows the stable task token only until run returns. */
XR_FUNC XrXirStatus xlsp_hierarchy_request_capture(XrLspServer *, const XrJsonValue *,
    unsigned, bool, XlspHierarchyRequest **);
XR_FUNC XrXirStatus xlsp_hierarchy_request_run(XlspHierarchyRequest *,
    const _Atomic bool *, XlspSourceRequestResult *);
XR_FUNC void xlsp_hierarchy_request_free(XlspHierarchyRequest *);
#endif // XLSP_HIERARCHY_REQUEST_H
