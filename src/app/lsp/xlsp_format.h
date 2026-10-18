/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_FORMAT_H
#define XLSP_FORMAT_H
#include "../../base/xjson.h"
typedef struct XrLspServer XrLspServer;
XR_FUNC XrJsonValue *xlsp_format_handle(XrLspServer *,XrJsonValue *);
#endif // XLSP_FORMAT_H
