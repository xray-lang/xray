/* Original producer and owned sender; only the producer symbol is renamed. */
XR_FUNC void xlsp_send_notification(XrLspServer *server, const char *method, XrJsonValue *params) {
    XrXirStatus status = xlsp_protocol_send_notification(server, method, params);
    if (status != XR_XIR_OK && server) {
        server->exit_received = true; lsp_log("Notification publication failed: %u", (unsigned)status);
    }
}

void baseline_clear_diagnostics(XrLspServer *server) {
    if (!server->doc_table)
        return;
    XrLspDocTable *table = server->doc_table;
    for (int i = 0; i < table->bucket_count; i++) {
        XrLspDocBucket *bucket = table->buckets[i];
        while (bucket) {
            XrLspDocument *doc = bucket->doc;
            if (doc) {
                XrJsonValue *params = xjson_new_object();
                xjson_object_set(params, "uri", xjson_new_string(doc->uri));
                xjson_object_set(params, "diagnostics", xjson_new_array());
                xlsp_send_notification(server, "textDocument/publishDiagnostics", params);
                xjson_free(params);
            }
            bucket = bucket->next;
        }
    }
}
