static void finish_request_handler(XrLspServer *server, const XlspRequestId *id,
    LspRequestHandler handler, XrJsonValue *result) {
    // Check if cancelled during execution
    if (pending_request_is_cancelled(server, id)) {
        lsp_log("Request %s was cancelled during execution",
                xlsp_request_id_debug(id));
        send_error(server, id, LSP_ERROR_REQUEST_CANCELLED, "Request cancelled");
        if (result)
            xjson_free(result);
    } else if (server->source_failure.stage) {
        char message[128];
        int code = handler == xlsp_handle_lc_shutdown &&
            server->source_failure.status == XR_XIR_BAD_STRUCTURE ? -32602 : -32603;
        snprintf(message, sizeof(message), "Source query failed (stage=%u status=%u)",
            server->source_failure.stage, server->source_failure.status);
        send_error(server, id, code, message);
        xjson_free(result);
    } else if (handler == xlsp_handle_td_formatting &&
               server->formatting_failure.stage) {
        char message[128];
        snprintf(message, sizeof(message), "Formatting failed (stage=%u status=%u)",
            server->formatting_failure.stage, server->formatting_failure.status);
        send_error(server, id, -32603, message);
        xjson_free(result);
    } else {
        send_response(server, id, result);
    }
}
