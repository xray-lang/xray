/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_request_cancel.h - Cancellation through admitted request task identity
 */
#ifndef XLSP_REQUEST_CANCEL_H
#define XLSP_REQUEST_CANCEL_H
#include "xlsp_server.h"

// Unknown IDs have no effect. A pending request without a worker is marked only
// locally. Worker cancellation uses its explicit task ID, never the protocol ID.
// The event thread owns pending state exclusively; no operation allocates.
XR_FUNC bool xlsp_request_cancel(XrLspServer *server, const XlspRequestId *id);
#endif // XLSP_REQUEST_CANCEL_H
