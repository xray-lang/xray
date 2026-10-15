/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_on_type.h - Strict on-type formatting request adapter
 *
 * KEY CONCEPT:
 *   Capture a bounded editor request before invoking the owned lexical service.
 */
#ifndef XLSP_ON_TYPE_H
#define XLSP_ON_TYPE_H
#include "../../base/xjson.h"
typedef struct XrLspServer XrLspServer;
XR_FUNC XrJsonValue *xlsp_on_type_handle(XrLspServer *,XrJsonValue *);
#endif // XLSP_ON_TYPE_H
