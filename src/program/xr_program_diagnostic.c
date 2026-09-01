/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_diagnostic.c - Shared XrProgram diagnostic formatting
 */

#include "xr_program_internal.h"

#include <stdarg.h>
#include <stdio.h>

void xr_program_set_diagnostic(char *diagnostic, size_t diagnostic_size, const char *format, ...) {
    va_list arguments;
    if (!diagnostic || diagnostic_size == 0)
        return;
    va_start(arguments, format);
    vsnprintf(diagnostic, diagnostic_size, format, arguments);
    va_end(arguments);
    diagnostic[diagnostic_size - 1] = '\0';
}
