/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_print.h - Deterministic scalar SSA text output
 */
#ifndef XLIR_PRINT_H
#define XLIR_PRINT_H
#include "xlir.h"

typedef bool (*XrLirWrite)(void *context, const char *bytes, size_t length);
/* Writes UTF-8 with LF, independent of the host text-file convention. A sink
 * failure stops output immediately; a partial stream must not be published. */
XR_FUNC XrLirStatus xr_lir_print(const XrLirModule *, XrLirWrite, void *context);

#endif  // XLIR_PRINT_H
