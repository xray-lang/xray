/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_script_info.c - Runtime-owned script metadata.
 */

#include "xr_script_info.h"
#include <stddef.h>

void xr_script_info_init(XrScriptInfo *info) {
    if (!info)
        return;
    info->file = NULL;
    info->argc = 0;
    info->argv = NULL;
}

void xr_script_info_set(XrScriptInfo *info, const char *file, int argc, char **argv) {
    if (!info)
        return;
    info->file = file;
    info->argc = argc;
    info->argv = argv;
}
