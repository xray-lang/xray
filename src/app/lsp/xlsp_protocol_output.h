/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_protocol_output.h - Owned complete JSON publication to a byte transport
 */
#ifndef XLSP_PROTOCOL_OUTPUT_H
#define XLSP_PROTOCOL_OUTPUT_H
#include "xlsp_server.h"
#include "../../base/xjson.h"
/* All functions consume their JSON argument on every path. Caller owns the
 * event thread and excludes server mutation. Preparation failures preserve
 * server state; a dead/failed transport sets exit_received. A successful write
 * means local transport completion, not a peer acknowledgement. */
XR_FUNC XrXirStatus xlsp_protocol_write_owned(XrLspServer *, XrJsonValue *message);
XR_FUNC XrXirStatus xlsp_protocol_send_request(XrLspServer *, const char *, XrJsonValue *params);
XR_FUNC XrXirStatus xlsp_protocol_send_notification(XrLspServer *, const char *, XrJsonValue *params);
#endif // XLSP_PROTOCOL_OUTPUT_H
