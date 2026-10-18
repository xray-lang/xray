/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_call_hierarchy.c - Implementation navigation through the Source owner
 */
#include "xlsp_call_hierarchy.h"
#include "xlsp_navigation.h"
XR_FUNC XrJsonValue *xlsp_handle_implementation(XrLspServer *server,XrJsonValue *params) {
    return xlsp_navigation_handle(server,params,0);
}
