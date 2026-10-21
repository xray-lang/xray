/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_native_main.inc.c - SDK-owned bridge to an actual generated program
 */
#include "execution/xr_xir_host_cli.h"
#include <stdio.h>
#include <string.h>
#ifndef XIR_SDK_PROGRAM_SYMBOL
#error "An actual generated Program symbol is required"
#endif
XR_DATA const XrXirProgramSpec XIR_SDK_PROGRAM_SYMBOL;
int main(int argc, char **argv) {
    bool sandbox = argc == 2 && strcmp(argv[1], "--sandbox") == 0;
    if (argc != 1 && !sandbox) {
        fprintf(stderr, "Usage: %s [--sandbox]\n", argv[0]);
        return 3;
    }
    return xr_xir_host_main(&XIR_SDK_PROGRAM_SYMBOL, sandbox);
}
