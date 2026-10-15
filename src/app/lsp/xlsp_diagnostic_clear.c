/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_diagnostic_clear.c - Publish complete empty diagnostics with one owner
 */
#include "xlsp_protocol_output.h"
#include "xlsp_document_store.h"
#include "xlsp_index_json.h"

XR_FUNC void xlsp_clear_all_diagnostics(XrLspServer *server) {
    if (!server || !server->doc_table || server->exit_received) return;
    XrLspDocTable *table = server->doc_table;
    for (int i = 0; i < table->bucket_count; ++i) {
        for (XrLspDocBucket *bucket = table->buckets[i]; bucket; bucket = bucket->next) {
            XrLspDocument *doc = bucket->doc;
            if (!doc) continue;
            XrJsonValue *params = xjson_new_object();
            if (!params || !doc->uri || !index_json_member(params, "uri", xjson_new_string(doc->uri)) ||
                !index_json_member(params, "diagnostics", xjson_new_array())) {
                xjson_free(params); server->exit_received = true; return;
            }
            // The sender consumes params after either publication or rejection.
            if (xlsp_protocol_send_notification(server, "textDocument/publishDiagnostics", params) != XR_XIR_OK) {
                server->exit_received = true; return;
            }
        }
    }
}
