/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_pending_requests.h - Event-thread ownership of pending request IDs
 */
#ifndef XLSP_PENDING_REQUESTS_H
#define XLSP_PENDING_REQUESTS_H
#include "xlsp_server.h"

// Success takes the ID; failure preserves both the table and the caller's ID.
// Method names remain alive until removal. No operation allocates memory.
XR_FUNC bool xlsp_pending_request_add(XlspPendingRequests *, XlspRequestId, const char *, uint64_t);
// The fixed-array entry is borrowed only until removal or server destruction.
XR_FUNC XlspPendingRequest *xlsp_pending_request_find(XlspPendingRequests *, const XlspRequestId *);
XR_FUNC bool xlsp_pending_request_remove(XlspPendingRequests *, const XlspRequestId *);
#endif // XLSP_PENDING_REQUESTS_H
