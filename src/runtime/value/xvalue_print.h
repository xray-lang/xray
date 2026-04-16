/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_print.h - Unified value printing system
 *
 * KEY CONCEPT:
 *   Single implementation for value printing.
 *   Supports USER, DEBUG, and REPR modes.
 */

#ifndef XVALUE_PRINT_H
#define XVALUE_PRINT_H

#include "xvalue.h"
#include <stdio.h>

/* ========== Core API ========== */

XR_FUNC void xr_value_print(XrValue value);
XR_FUNC void xr_value_println(XrValue value);
XR_FUNC void xr_value_fprint(FILE *stream, XrValue value);

/* ========== Formatted Dump ========== */

XR_FUNC void xr_value_dump(XrValue value, int indent);

#endif // XVALUE_PRINT_H
