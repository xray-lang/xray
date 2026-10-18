/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_request_cancel.c - Cancellation through admitted request task identity
 */
#include "xlsp_request_cancel.h"
#include "xlsp_pending_requests.h"
#include "xlsp_async.h"

XR_FUNC bool xlsp_request_cancel(XrLspServer *server, const XlspRequestId *id) {
    if (!server) return false;
    XlspPendingRequest *pending = xlsp_pending_request_find(&server->pending_requests, id);
    if (!pending) return false;
    pending->cancelled = true;
    if (pending->source_task_id && server->async)
        xlsp_async_cancel_task(server->async, pending->source_task_id);
    return true;
}
