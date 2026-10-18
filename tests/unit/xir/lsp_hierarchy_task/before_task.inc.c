/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_task.c - Owned Source task admission and completion
 */
#include "xlsp_source_task.h"
#include "xlsp_source_completion.h"
#include "xlsp_pending_requests.h"
#include "xlsp_source_request.h"
#include "xlsp_navigation.h"
#include "xlsp_async.h"
#include "../../base/xmalloc.h"

typedef struct XlspSourceTask {
    XrLspServer *server;
    XlspRequestId id;
    XlspSourceRequest *request;
    XrLspTask *task;
    XlspSourceRequestResult result;
    uint64_t revision;
} XlspSourceTask;

static void source_task_free(XlspSourceTask *source) {
    if (!source) return;
    xlsp_source_request_free(source->request); xlsp_request_id_free(&source->id); xr_free(source);
}
static void source_task_execute(void *data) {
    XlspSourceTask *source = data;
    XrXirStatus status = xlsp_source_request_run(source->request, &source->task->cancel_requested, &source->result);
    if (status != XR_XIR_OK) { source->result.status = status; source->result.stage = 1; }
    source->task->result = source;
}
static void source_task_complete(void *data, void *result) {
    XlspSourceTask *source = data;
    XlspSourceCompletion completion = {.id = &source->id, .task_id = source->task->task_id,
        .revision = source->revision, .worker_completed = result != NULL};
    // The terminal owner closes on output failure and consumes obsolete results.
    xlsp_source_completion_publish(source->server, &completion, &source->result);
    source_task_free(source);
}

XR_FUNC XrXirStatus xlsp_source_task_submit(XrLspServer *server, const XlspRequestId *id,
                                           XrJsonValue *params, unsigned mode) {
    if (!server || !xlsp_request_id_valid(id) || mode > 3) return XR_XIR_BAD_STRUCTURE;
    if (!server->async || !server->initialized || server->shutdown_received || server->exit_received)
        return XR_XIR_BAD_STAGE;
    if (server->source_revision == UINT64_MAX) return XR_XIR_BUDGET;
    XlspPendingRequest *pending = xlsp_pending_request_find(&server->pending_requests, id);
    if (!pending || pending->source_task_id || pending->cancelled) return XR_XIR_BAD_STAGE;
    XlspSourceTask *source = xr_calloc(1, sizeof(*source)); if (!source) return XR_XIR_OUT_OF_MEMORY;
    source->server = server; source->revision = server->source_revision;
    XrXirStatus status = XR_XIR_OUT_OF_MEMORY;
    if (xlsp_request_id_clone(id, &source->id) != XLSP_PROTOCOL_OK) goto failed;
    status = xlsp_navigation_capture(server, params, mode, &source->request);
    if (status != XR_XIR_OK) goto failed;
    source->task = xlsp_task_new(source_task_execute, source_task_complete, source, XLSP_TASK_NORMAL);
    if (!source->task) { status = XR_XIR_OUT_OF_MEMORY; goto failed; }
    uint64_t task_id = xlsp_async_submit(server->async, source->task);
    if (!task_id) { xlsp_task_free(source->task); status = XR_XIR_BAD_STAGE; goto failed; }
    pending->source_task_id = task_id; return XR_XIR_OK;
failed:
    source_task_free(source); return status;
}
