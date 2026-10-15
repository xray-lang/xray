/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_task.h - Owned Source task admission and completion
 */
#ifndef XLSP_SOURCE_TASK_H
#define XLSP_SOURCE_TASK_H
#include "xlsp_server.h"

// The event thread reserves a pending ID before admission. Success captures all
// editor inputs and binds the actual worker task; failure leaves the slot owned
// by the caller. Server lifetime extends through async shutdown and completion.
// Modes: 0 definition, 1 references, 2 highlights, 3 hover, 4-6 call hierarchy
// prepare/incoming/outgoing, 7-9 type hierarchy prepare/supertypes/subtypes.
XR_FUNC XrXirStatus xlsp_source_task_submit(XrLspServer *, const XlspRequestId *, XrJsonValue *, unsigned mode);
#endif // XLSP_SOURCE_TASK_H
