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
// Mode 10 produces CodeLens reference counts from the same captured corpus.
// Modes 11-13 produce semantic full/delta/range; successful terminal output
// commits full/delta cache proposals exclusively on the dispatch thread.
// Mode 14 resolves document links from captured texts and project authority.
// Mode 15 produces signature help from parser boundaries and Checked Source.
// Mode 16 prepares binding type/parameter hints; public dispatch requires the full
// type and effect hint contract before switching its request boundary.
XR_FUNC XrXirStatus xlsp_source_task_submit(XrLspServer *, const XlspRequestId *, XrJsonValue *, unsigned mode);
#endif // XLSP_SOURCE_TASK_H
