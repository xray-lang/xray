/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xenum_builtins.h - Enum builtin methods
 *
 * KEY CONCEPT:
 *   User enum values are tag+payload aggregates. These helpers only serve the
 *   cold constructor metadata path used before payload construction.
 */

#ifndef XENUM_BUILTINS_H
#define XENUM_BUILTINS_H

#include "../value/xvalue.h"

/* ========== Enum Constructor Metadata Helpers ========== */

// Status.Success.name -> "Success"
XR_FUNC XrValue xr_enum_get_name(XrVMRuntime *isolate, XrValue *args, int nargs);

// Status.Success.ordinal -> 0
XR_FUNC XrValue xr_enum_get_ordinal(XrVMRuntime *isolate, XrValue *args, int nargs);

// Status.Success.toString() -> "Status.Success"
XR_FUNC XrValue xr_enum_toString(XrVMRuntime *isolate, XrValue *args, int nargs);

#endif  // XENUM_BUILTINS_H
