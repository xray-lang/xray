/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_completion.c - Terminal publication for an admitted Source task
 */
#include "xlsp_source_completion.h"
#include "xlsp_pending_requests.h"
#include "xlsp_protocol_output.h"
#include <stdio.h>

static XrXirStatus publish_result(XrLspServer *server, const XlspSourceCompletion *completion,
                                 const XlspPendingRequest *pending, XlspSourceRequestResult *result,
                                 XrJsonValue **json) {
    int code = 0; const char *description = NULL; char failure[128];
    if (!completion->worker_completed || result->cancelled || pending->cancelled) {
        code = -32800; description = "Request cancelled";
    } else if (server->source_revision != completion->revision) {
        code = -32803; description = "Document state changed during Source query";
    } else if (result->status != XR_XIR_OK) {
        code = -32603;
        snprintf(failure, sizeof(failure), "Source query failed (stage=%u status=%u)",
                 result->stage, (unsigned)result->status);
        description = failure;
    } else if (!*json) {
        code = -32603; description = "Source query returned no owned result";
    }
    XrJsonValue *message = NULL;
    XlspProtocolStatus built = code ? xlsp_protocol_error(completion->id, code, description, &message) :
        xlsp_protocol_response(completion->id, json, &message);
    if (built != XLSP_PROTOCOL_OK)
        return built == XLSP_PROTOCOL_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BAD_STRUCTURE;
    return xlsp_protocol_write_owned(server, message);
}

XR_FUNC XrXirStatus xlsp_source_completion_publish(XrLspServer *server, const XlspSourceCompletion *completion,
                                                  XlspSourceRequestResult *result) {
    XrJsonValue *json = result ? result->json : NULL;
    if (result) result->json = NULL;
    XrXirStatus status = XR_XIR_BAD_STRUCTURE;
    if (!server || !completion || !result || !completion->task_id ||
        !xlsp_request_id_valid(completion->id)) goto done;
    XlspPendingRequest *pending = xlsp_pending_request_find(&server->pending_requests, completion->id);
    if (!pending || pending->source_task_id != completion->task_id) { status = XR_XIR_BAD_STAGE; goto done; }
    status = server->exit_received ? XR_XIR_OK : publish_result(server, completion, pending, result, &json);
    if (status != XR_XIR_OK) server->exit_received = true;
    if (!xlsp_pending_request_remove(&server->pending_requests, completion->id)) {
        server->exit_received = true; status = XR_XIR_BAD_STAGE;
    }
done:
    xjson_free(json); return status;
}
