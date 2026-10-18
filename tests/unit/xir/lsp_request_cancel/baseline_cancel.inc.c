static bool pending_request_cancel(XrLspServer *server, const XlspRequestId *id) {
    XlspPendingRequests *pending = &server->pending_requests;
    bool found = false,source=false;

    for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
        if (xlsp_request_id_equals(&pending->requests[i].id, id)) {
            pending->requests[i].cancelled = true;
            source=pending->requests[i].source_task_id!=0;
            if(source&&server->async)xlsp_async_cancel_task(server->async,pending->requests[i].source_task_id);
            lsp_log("Request %s (%s) marked as cancelled", xlsp_request_id_debug(id),
                    pending->requests[i].method ? pending->requests[i].method : "unknown");
            found = true;
            break;
        }
    }

    if (!source && server->async && id && id->kind == XLSP_ID_NUMBER) {
        int async_cancelled = xlsp_async_cancel_request(server->async, (int64_t) id->as.number);
        if (async_cancelled > 0) {
            lsp_log("Cancelled %d background tasks for request %s", async_cancelled,
                    xlsp_request_id_debug(id));
        }
    }

    if (!found) {
        lsp_log("Request %s not found for cancellation", xlsp_request_id_debug(id));
    }
    return found;
}
