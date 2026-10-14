/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_protocol_message.h - Owned request identities and atomic responses
 */
#ifndef XLSP_PROTOCOL_MESSAGE_H
#define XLSP_PROTOCOL_MESSAGE_H
#include "../../base/xjson.h"
#include <stdint.h>
typedef enum XlspRequestIdKind {XLSP_ID_NONE=0,XLSP_ID_NUMBER,XLSP_ID_STRING} XlspRequestIdKind;
typedef struct XlspRequestId {
    XlspRequestIdKind kind;
    size_t string_length;
    union {int32_t number;char *string;} as;
} XlspRequestId;
typedef enum XlspProtocolStatus {
    XLSP_PROTOCOL_OK=0,XLSP_PROTOCOL_BAD_ARGUMENT,XLSP_PROTOCOL_OUT_OF_MEMORY
} XlspProtocolStatus;
/* Missing id is a notification. Present ids must be a protocol integer or
 * valid UTF-8 string; strings own their full length, including embedded NUL.
 * Parse/clone publish only into an empty id, preserving it on every failure. */
XR_FUNC XlspProtocolStatus xlsp_request_id_parse(const XrJsonValue *,XlspRequestId *);
XR_FUNC XlspProtocolStatus xlsp_request_id_clone(const XlspRequestId *,XlspRequestId *);
XR_FUNC void xlsp_request_id_free(XlspRequestId *);
XR_FUNC bool xlsp_request_id_valid(const XlspRequestId *);
XR_FUNC bool xlsp_request_id_equals(const XlspRequestId *,const XlspRequestId *);
XR_FUNC XrJsonValue *xlsp_request_id_to_json(const XlspRequestId *);
XR_FUNC const char *xlsp_request_id_debug(const XlspRequestId *);
/* Success moves *result into the response; failure preserves both outputs.
 * A missing result becomes JSON null. NONE is allowed only for error ids. */
XR_FUNC XlspProtocolStatus xlsp_protocol_response(const XlspRequestId *,XrJsonValue **,XrJsonValue **);
XR_FUNC XlspProtocolStatus xlsp_protocol_error(const XlspRequestId *,int32_t,const char *,XrJsonValue **);
/* An empty id creates a notification; otherwise it must be valid. Params are
 * an optional object/array and transfer only with the complete call envelope. */
XR_FUNC XlspProtocolStatus xlsp_protocol_call(const XlspRequestId *,const char *,XrJsonValue **,XrJsonValue **);
#endif // XLSP_PROTOCOL_MESSAGE_H
