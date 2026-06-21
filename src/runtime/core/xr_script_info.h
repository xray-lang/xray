/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_script_info.h - Runtime-owned script metadata.
 */

#ifndef XR_SCRIPT_INFO_H
#define XR_SCRIPT_INFO_H

#include "../../base/xdefs.h"

typedef struct XrScriptInfo {
    const char *file;
    int argc;
    char **argv;
} XrScriptInfo;

XR_FUNC void xr_script_info_init(XrScriptInfo *info);
XR_FUNC void xr_script_info_set(XrScriptInfo *info, const char *file, int argc, char **argv);

#endif  // XR_SCRIPT_INFO_H
