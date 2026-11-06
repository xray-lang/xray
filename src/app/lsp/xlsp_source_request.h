/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_SOURCE_REQUEST_H
#define XLSP_SOURCE_REQUEST_H
#include "xlsp_source_workspace.h"
#include <stdatomic.h>
typedef struct XlspSourceRequest XlspSourceRequest;
typedef struct XlspTokenUpdate XlspTokenUpdate;
typedef struct XlspSourceRequestInput {
    const XrXirCompileContext *context;
    const XlspSourceDocument *documents;
    size_t count,entry;
    XrLspPosition position;
    unsigned mode;
    bool include_declaration;
} XlspSourceRequestInput;
typedef struct XlspSourceRequestResult {
    XrXirStatus status;
    unsigned stage;
    bool cancelled;
    XrJsonValue *json;
    /* Owned terminal cache proposal, consumed together with JSON publication. */
    XlspTokenUpdate *token_update;
} XlspSourceRequestResult;
/* Capture owns every URI, text and version on the original ledger. Only capture
 * borrows editor state; the one-shot worker run never accesses the document table.
 * Cancellation is checked at admission, explicit build stage boundaries,
 * the actual parser Session's safe points and before publication. The task
 * token and stack control remain live until workspace/Session destruction
 * inside run; neither callback nor Session escapes into query snapshots.
 * A cancelled result's flag takes precedence over its underlying stage status. */
XR_FUNC XrXirStatus xlsp_source_request_new(const XlspSourceRequestInput *,XlspSourceRequest **);
XR_FUNC void xlsp_source_request_free(XlspSourceRequest *);
XR_FUNC XrXirStatus xlsp_source_request_run(XlspSourceRequest *,const _Atomic bool *,XlspSourceRequestResult *);
#endif // XLSP_SOURCE_REQUEST_H
