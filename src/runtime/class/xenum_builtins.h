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
 *   Make enum a true builtin class.
 *   Supports: name, value, ordinal, toString.
 */

#ifndef XENUM_BUILTINS_H
#define XENUM_BUILTINS_H

#include "../value/xvalue.h"


/* ========== Enum Instance Methods ========== */

// Status.Success.name -> "Success"
XR_FUNC XrValue xr_enum_get_name(XrayIsolate *isolate, XrValue *args, int nargs);

// Status.Success.value -> 200
XR_FUNC XrValue xr_enum_get_value(XrayIsolate *isolate, XrValue *args, int nargs);

// Status.Success.ordinal -> 0
XR_FUNC XrValue xr_enum_get_ordinal(XrayIsolate *isolate, XrValue *args, int nargs);

// Status.Success.toString() -> "Status.Success"
XR_FUNC XrValue xr_enum_toString(XrayIsolate *isolate, XrValue *args, int nargs);

/* ========== EnumType Class Methods ========== */

// Status.memberCount -> 3
XR_FUNC XrValue xr_enum_type_get_member_count(XrayIsolate *isolate, XrValue *args, int nargs);

// Status.getMember(0) -> Status.OK
XR_FUNC XrValue xr_enum_type_get_member(XrayIsolate *isolate, XrValue *args, int nargs);

#endif // XENUM_BUILTINS_H
