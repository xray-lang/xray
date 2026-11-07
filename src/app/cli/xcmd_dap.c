/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_dap.c - Report an unavailable command capability
 */
#include "xcli.h"

XR_FUNC int cmd_dap(const XrCliInvocation *inv) {
    if (!inv) return XR_CLI_EXIT_INTERNAL;
    xr_cli_error("dap", "temporarily unavailable");
    return XR_CLI_EXIT_FAIL;
}
