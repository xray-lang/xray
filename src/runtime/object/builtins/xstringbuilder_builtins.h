/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstringbuilder_builtins.h - StringBuilder builtin methods
 */

#ifndef XSTRINGBUILDER_BUILTINS_H
#define XSTRINGBUILDER_BUILTINS_H

#include "xvalue.h"
#include "xdefs.h"

// Constructor
XR_FUNC XrValue xr_builtin_stringbuilder_new(XrayIsolate *isolate, XrValue *args, int nargs);

// Instance methods
XR_FUNC XrValue xr_builtin_stringbuilder_append(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_stringbuilder_toString(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_stringbuilder_clear(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_builtin_stringbuilder_length(XrayIsolate *isolate, XrValue *args, int nargs);

// Create StringBuilder class (using XrClassBuilder)
typedef struct XrClass XrClass;
XR_FUNC XrClass* xr_stringbuilder_create_class(XrayIsolate *X, XrClass *objectClass);

#endif // XSTRINGBUILDER_BUILTINS_H
