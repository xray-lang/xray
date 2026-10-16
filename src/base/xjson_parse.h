/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XJSON_PARSE_H
#define XJSON_PARSE_H
#include "xjson.h"
typedef enum XrJsonParseStatus {
    XR_JSON_PARSE_OK,
    XR_JSON_PARSE_INVALID_JSON,
    XR_JSON_PARSE_OUT_OF_MEMORY,
    XR_JSON_PARSE_BAD_ARGUMENT
} XrJsonParseStatus;
/* Parse one complete UTF-8 JSON slice under XJSON_MAX_DEPTH. The output must be
 * empty and is published only on success. Invalid grammar, raw control bytes,
 * malformed Unicode and depth exhaustion are INVALID_JSON. Allocation failure
 * wins when it prevents further parsing; no prevalidation pass is performed.
 * The ordinary pointer-returning API uses this same parser and ownership. */
XR_FUNC XrJsonParseStatus xjson_parse_status(const char *,size_t,XrJsonValue **);
#endif // XJSON_PARSE_H
