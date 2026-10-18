/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_method_table.h - Atomic per-server method table ownership
 */
#ifndef XLSP_METHOD_TABLE_H
#define XLSP_METHOD_TABLE_H
#include "xlsp_server.h"

typedef XrJsonValue *(*LspRequestHandler)(XrLspServer *, XrJsonValue *);
typedef void (*LspNotificationHandler)(XrLspServer *, XrJsonValue *);
typedef struct {
    const char *method;
    LspRequestHandler request_handler;
    LspNotificationHandler notification_handler;
} LspMethodEntry;

// Entries and names remain immutable and alive until table destruction.
// The event thread owns the server exclusively. Failure preserves all fields.
XR_FUNC XrXirStatus xlsp_method_table_initialize(XrLspServer *server,
                                               const LspMethodEntry *entries, size_t count);
XR_FUNC const LspMethodEntry *xlsp_method_table_find(const XrLspServer *server, const char *method);
XR_FUNC void xlsp_method_table_free(XrLspServer *server);
#endif // XLSP_METHOD_TABLE_H
