/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_repl.c - Report an unavailable command capability
 */
#include "xcli.h"

XR_FUNC int cmd_repl(const XrCliInvocation *inv) {
    if (!inv) return XR_CLI_EXIT_INTERNAL;
    xr_cli_error("repl", "temporarily unavailable");
    return XR_CLI_EXIT_FAIL;
}
