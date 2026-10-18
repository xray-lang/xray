/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_completion.h - Terminal publication for an admitted Source task
 */
#ifndef XLSP_SOURCE_COMPLETION_H
#define XLSP_SOURCE_COMPLETION_H
#include "xlsp_server.h"
#include "xlsp_source_request.h"

typedef struct XlspSourceCompletion {
    const XlspRequestId *id;
    uint64_t task_id, revision;
    bool worker_completed;
} XlspSourceCompletion;

// Consumes result->json on every path. Only the matching task may retire a slot.
// Shutdown permits replies to admitted requests; exit permits cleanup only.
XR_FUNC XrXirStatus xlsp_source_completion_publish(XrLspServer *, const XlspSourceCompletion *,
                                                  XlspSourceRequestResult *);
#endif // XLSP_SOURCE_COMPLETION_H
